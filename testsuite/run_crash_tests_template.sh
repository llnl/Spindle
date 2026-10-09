#!/usr/bin/env bash

# Usage:
#   ./run_crash_tests.sh [--launcher=serial|flux|slurm|slurm-plugin] --nodes=N
#                        [--scratch=DIR] [--modes=LIST]
#                        [--session | --cross-exe | --orig-path]
#
# By default, runs all normal crash tests; use --modes to specify a subset to run.
# --session, --cross-exe and --orig-path instead run the session-based tests;
# these are separate because they require fresh sessions
#
# If running the tests from a non-shared filesystem, set --scratch to a shared
# filesystem so that the script can count all corefiles produced across all nodes.
# The CI containers' main filesystem is not shared, so a shared volume should be
# mounted across all containers and specified in --scratch

set -u

LAUNCHER="TEST_RESOURCE_MANAGER"
NODES=""
CRASH_TEST_SCRATCH="${CRASH_TEST_SCRATCH:-}"
SPINDLE="${SPINDLE:-SPINDLE_EXEC}"
SPINDLE_RC="${SPINDLE_RC:-SPINDLE_RC_PATH}"
TESTDIR="${TESTDIR:-TEST_RUN_DIR}"

die() { echo "FAIL: $*" >&2; exit 1; }

# The tests to run
# Fields:
#  mode: --mode to pass to crash test runner
#  cores: expected number of cores produced; if N, then equal to total number of ranks
#  crashers: expected number of crash log rows
#    N = all ranks, 2N = two per rank, E = even ranks (ceil(N/2)), or a
#    literal count
#  flags (comma-separated): "multi-rank" skips the mode on a single rank;
#    "clean" expects the test to NOT crash; "altstack" runs the mode with
#    --crash-altstack; "nofollowfork" runs it with --follow-fork=false;
#    "nocrashhandler" runs it without the crash handler;
#    "forkchild" means rows may come from fork or exec children of a rank.
#  top_frame_regex: regex that should match the top frame in produced coredumps.
#    Note that all threads will be checked, so in multithreaded examples the regex should
#    also match anything that could be on threads other than the one that faulted.
#  site_regex: optional regex for the site column of the crash log; 
#    if present, every logged site must match the regex for the test to pass
#  binary: optional alternate executable to run in place of the default crash_test.
#  crash_mode: optional alternate crash mode argument to executable
CRASH_TESTS=(
# mode                              ; cores ; crashers ; flags             ; top_frame_regex                              ; site_regex                                   ; binary               ; crash_mode
 'all-same                          ; 1     ; N        ;                   ; crash_function_A                             ; crash_test\+0x    '
 'fixed-address-exe                 ; 1     ; N        ;                   ; crash_function_A                             ; crash_test_fixedaddr\+0x                     ; crash_test_fixedaddr ; all-same'
 'pie-exe                           ; 1     ; N        ;                   ; crash_function_A                             ; crash_test_pie\+0x                           ; crash_test_pie       ; all-same'
 'all-different                     ; N     ; N        ;                   ; crash_function_[0-9]+'
 'two-groups                        ; 2     ; N        ;                   ; crash_function_(A|B)'
 'one-crashes                       ; 1     ; 1        ;                   ; crash_function_A'
 'partial                           ; 1     ; E        ;                   ; crash_function_A'
 'late-straggler                    ; 1     ; N        ;                   ; crash_function_A'
 'in-library                        ; 1     ; N        ;                   ; crash_in_library                             ; libcrashfuncs\.so\+0x    '
 'in-dlmopen-library                ; 1     ; N        ;                   ; crash_in_library                             ; libcrashfuncs\.so\+0x    '
 'in-fixed-library                  ; 1     ; N        ;                   ; crash_in_fixed_library                       ; libcrashfixed\.so\+0x    '
 'in-fixed-dlmopen-library          ; 1     ; N        ;                   ; crash_in_fixed_library                       ; libcrashfixed\.so\+0x    '
 'in-library-ctor                   ; 1     ; 1        ;                   ; ctor_crash'
 'sigabrt                           ; 1     ; N        ;                   ; (__GI_)?raise|abort|pthread_kill'
 'assert                            ; 1     ; N        ;                   ; (__GI_)?raise|abort|pthread_kill             ; ^abort:.*Assertion    '
 'mixed-abort-segv                  ; 2     ; N        ; multi-rank        ; (__GI_)?raise|abort|pthread_kill|do_mixed_abort_segv'
 'kill-segv                         ; 1     ; N        ;                   ; kill|do_kill_segv                            ; libc\.so.*\+0x    '
 'span-read                         ; 1     ; N        ;                   ; do_span_read'
 'safepoint                         ; 0     ; 0        ; clean             ; -'
 'safepoint-then-crash              ; 1     ; N        ;                   ; crash_function_A'
 'safepoint-bad                     ; 1     ; N        ;                   ; do_safepoint_bad'
 'safepoint-bad-write               ; 1     ; N        ;                   ; do_safepoint_bad_write'
 'safepoint-fix-write               ; 0     ; 0        ; clean             ; -'
 'safepoint-fix-write-altstack      ; 0     ; 0        ; clean,altstack    ; -                                            ;                                              ;                      ; safepoint-fix-write'
 'safepoint-longjmp                 ; 0     ; 0        ; clean             ; -'
 'safepoint-nocrashhandler          ; 0     ; 0        ; clean,nocrashhandler ; -                                         ;                                              ;                      ; safepoint'
 'safepoint-span-read               ; 0     ; 0        ; clean             ; -'
 'safepoint-span-write              ; 0     ; 0        ; clean             ; -'
 'safepoint-span-bad-read           ; 1     ; N        ;                   ; do_safepoint_span_bad_read'
 'safepoint-span-bad-write          ; 1     ; N        ;                   ; do_safepoint_span_bad_write'
 'mmap-sigbus-bad                   ; 1     ; N        ;                   ; do_mmap_sigbus_bad'
 'mmap-sigbus-fixed                 ; 0     ; 0        ; clean             ; -'
 'chained-kill-segv                 ; 0     ; 0        ; clean             ; -'
 'ignored-kill-segv                 ; 0     ; 0        ; clean             ; -'
 'ignored-siginfo-kill-segv         ; 0     ; 0        ; clean             ; -'
 'default-siginfo-kill-segv         ; 1     ; N        ;                   ; kill|do_default_siginfo_kill_segv            ; libc\.so.*\+0x    '
 'fork-child-prereconnect           ; 1     ; N        ;                   ; crash_function_A                             ; crash_test\+0x    '
 'fork-child-reconnect              ; 1     ; N        ;                   ; crash_function_A                             ; crash_test\+0x    '
 'fork-child-nofollow               ; 1     ; N        ; nofollowfork      ; crash_function_A                             ; crash_test\+0x    '
 'fork-child-inherited-safepoint    ; 0     ; 0        ; clean             ; -'
 'fork-exec-child-crash             ; 1     ; N        ; forkchild         ; crash_function_A                             ; crash_test\+0x    '
 'fork-child-reconnect-then-parent  ; 1     ; 2N       ; forkchild         ; crash_function_A                             ; crash_test\+0x    '
 'no-crash                          ; 0     ; 0        ; clean             ; -'
)

declare -A TEST_CORES TEST_CRASHERS TEST_FLAGS TEST_TOPFRAME TEST_SITE TEST_BINARY TEST_CRASHMODE
declare -A LOG_SITES
DEFAULT_MODES=()
SESSION_TEST=""

# ---------------- test table parsing ----------------

trim() {
   local s="$1"
   s="${s#"${s%%[![:space:]]*}"}"
   s="${s%"${s##*[![:space:]]}"}"
   printf '%s' "$s"
}

parse_table() {
   local row mode cores crashers flags top site bin cmode
   for row in "${CRASH_TESTS[@]}"; do
      IFS=';' read -r mode cores crashers flags top site bin cmode <<<"$row"
      mode=$(trim "$mode")
      [ -n "$mode" ] || continue
      cores=$(trim "$cores")
      crashers=$(trim "$crashers")
      flags=$(trim "$flags")
      top=$(trim "$top")
      site=$(trim "$site")
      bin=$(trim "$bin")
      cmode=$(trim "$cmode")
      [ -n "$bin" ]   || bin="crash_test"
      [ -n "$cmode" ] || cmode="$mode"
      TEST_CORES[$mode]="$cores"
      TEST_CRASHERS[$mode]="$crashers"
      TEST_FLAGS[$mode]="$flags"
      TEST_TOPFRAME[$mode]="$top"
      TEST_SITE[$mode]="$site"
      TEST_BINARY[$mode]="$bin"
      TEST_CRASHMODE[$mode]="$cmode"
      DEFAULT_MODES+=("$mode")
   done
}

has_flag() {
   case ",${TEST_FLAGS[$1]:-}," in
      *,"$2",*) return 0 ;;
      *)        return 1 ;;
   esac
}

resolve_cores() {
   local mode="$1" val
   if [ "$LAUNCHER" = "serial" ]; then
      printf '1'
      return
   fi
   val="${TEST_CORES[$mode]}"
   [ "$val" = "N" ] && val="$NODES"
   printf '%s' "$val"
}

# Expected number of crashing ranks
resolve_crashers() {
   local mode="$1" val
   val="${TEST_CRASHERS[$mode]}"
   case "$val" in
      N)  val="$NODES" ;;
      2N) val=$(( 2 * NODES )) ;;
      E)  val=$(( (NODES + 1) / 2 )) ;;
   esac
   printf '%s' "$val"
}

# ---------------- arguments ----------------

usage() {
   local prog
   prog=$(basename "$0")
   cat <<EOF
Usage:
  $prog [--launcher=serial|flux|slurm|slurm-plugin] [--nodes=N]
        [--scratch=DIR] [--modes=mode1,mode2,...]
        [--session | --cross-exe | --orig-path]

Runs tests of the crash handler.

Options:
  --launcher=LAUNCHER  Resource manager to launch under: serial, flux, slurm, or
                       slurm-plugin.
  --nodes=N            Number of nodes/ranks to run on.
  --scratch=DIR        Directory where coredumps will be written.
                       When running on multiple nodes, this must be on a
                       shared filesystem.
  --modes=LIST         Comma-separated subset of modes to run (default: all).
  --session            Run the session mode crash log test instead of the
                       normal tests.
  --cross-exe          Run the cross-executable dedup test instead of the
                       normal tests.
  --orig-path          Run the original path dedup test instead of the
                       normal tests (needs at least 3 nodes).
  --help, -h           Show this help and exit.

Available tests:
EOF
   local mode
   for mode in "${DEFAULT_MODES[@]}"; do
      printf '  %s\n' "$mode"
   done
}

parse_args() {
   MODES=""
   for a in "$@"; do
      case "$a" in
         --launcher=*) LAUNCHER="${a#*=}" ;;
         --nodes=*)    NODES="${a#*=}"    ;;
         --scratch=*)  CRASH_TEST_SCRATCH=`realpath ${a#*=}`  ;;
         --modes=*)    MODES="${a#*=}"    ;;
         --session|--cross-exe|--orig-path)
            [ -z "$SESSION_TEST" ] || die "--session, --cross-exe and --orig-path are mutually exclusive"
            SESSION_TEST="${a#--}" ;;
         --help|-h)    usage; exit 0 ;;
         *) die "unknown argument '$a'" ;;
      esac
   done
}

check_prereqs() {
   local cp
   cp=$(cat /proc/sys/kernel/core_pattern)
   if [[ "${cp:0:1}" == "|" ]]; then
      die "crash tests can't run because core_pattern is set to a pipe"
   fi
   if [[ "${cp:0:1}" == "/" ]]; then
      die "crash tests can't run because core_pattern is an absolute path"
   fi
   if [[ "$cp" != *"%p"* ]]; then
      die "crash tests can't run because core_pattern does not include pid"
   fi

   command -v gdb >/dev/null 2>&1 || \
      die "crash tests can't run because gdb is not on path"

   case "$LAUNCHER" in
      serial|flux|slurm|slurm-plugin) ;;
      *) die "unknown launcher" ;;
   esac

   if [ "$LAUNCHER" = "serial" ]; then
      NODES=1
   elif [ -z "$NODES" ]; then
      die "--nodes required"
   elif ! [[ "$NODES" =~ ^[1-9][0-9]*$ ]]; then
      die "--nodes must be a positive integer (was '$NODES')"
   fi

   test -x "$TESTDIR/crash_test"            || die "can't find crash test executable"
   test -x "$TESTDIR/crash_test_fixedaddr"  || die "can't find crash_test_fixedaddr executable"
   test -x "$TESTDIR/crash_test_pie"        || die "can't find crash_test_pie executable"
   test -f "$TESTDIR/libcrashfuncs.so"      || die "can't find libcrashfuncs.so"
   test -f "$TESTDIR/libcrashfixed.so"      || die "can't find libcrashfixed.so"
}

# ---------------- test launching helpers ----------------

launch() {
   local mode="$1"
   local logfile="$2"
   local binary="$TESTDIR/${TEST_BINARY[$mode]:-crash_test}"
   local crash_mode="${TEST_CRASHMODE[$mode]:-$mode}"
   local cmdline_opts="--crash-dedup --crash-log=$logfile"
   local flux_opts=(-o spindle.crash-dedup -o spindle.crash-log="$logfile")
   if has_flag "$mode" altstack; then
      cmdline_opts="$cmdline_opts --crash-altstack"
      flux_opts+=(-o spindle.crash-altstack)
   fi
   if has_flag "$mode" nofollowfork; then
      cmdline_opts="$cmdline_opts --follow-fork=false"
      flux_opts+=(-o spindle.follow-fork=no)
   fi
   if has_flag "$mode" nocrashhandler; then
      cmdline_opts=""
      flux_opts=(-o spindle)
      export SPINDLE_NO_CRASH_DEDUP=1
   fi
   ulimit -c unlimited
   # Make libcrashfuncs.so visible to dlopen() from the mode's scratch dir.
   export LD_LIBRARY_PATH="$TESTDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
   case "$LAUNCHER" in
      serial)
         "$SPINDLE" \
            --no-mpi $cmdline_opts -- \
            "$binary" --crash-mode "$crash_mode"
         ;;
      flux)
         flux run \
            -o userrc="$SPINDLE_RC" \
            "${flux_opts[@]}" \
            -N"$NODES" -n"$NODES" \
            --env=LD_LIBRARY_PATH \
            -- "$binary" --crash-mode "$crash_mode"
         ;;
      slurm)
         salloc -N"$NODES" -n"$NODES" \
            "$SPINDLE" $cmdline_opts -- \
               srun "$binary" --crash-mode "$crash_mode"
         ;;
      slurm-plugin)
         salloc -N"$NODES" -n"$NODES" \
            srun "--spindle${cmdline_opts:+=$cmdline_opts}" \
               "$binary" --crash-mode "$crash_mode"
         ;;
   esac
}

# ---------------- coredump inspection ----------------

core_files() {
   local dir="$1"
   local cp glob search_dir
   cp=$(cat /proc/sys/kernel/core_pattern)
   case "$cp" in
      /*) glob=$(basename "$cp"); search_dir=$(dirname "$cp") ;;
      *)  glob="$cp";             search_dir="$dir" ;;
   esac
   # This converts any parameters (%p, %e, etc.) in the core pattern
   # into a shell glob so we can match all produced core files
   # (for example, "core.%e.%p" becomes "core.*.*")
   glob=$(echo "$glob" | sed -E 's|%[-0-9]*[peghistuclIPCsdfkrSTK]|*|g')
   find "$search_dir" -maxdepth 3 -type f -name "$glob" 2>/dev/null
}

count_cores() {
   core_files "$1" | wc -l
}

verify_top_frames() {
   local mode="$1"
   local dir="$2"
   local expected_top="${TEST_TOPFRAME[$mode]:-}"

   [ "$expected_top" = "-" ] && return 0
   [ -z "$expected_top" ] && return 0

   local binary="$TESTDIR/${TEST_BINARY[$mode]:-crash_test}"
   local core top cores
   cores=$(core_files "$dir")
   for core in $cores; do
      [ -e "$core" ] || continue
      top=$(gdb -batch -nx \
         -iex 'set print demangle off' \
         -ex 'set pagination off' \
         -ex 'bt 1' "$binary" "$core" 2>/dev/null \
         | grep -oE '#0\s+.*' | head -1)
      if ! echo "$top" | grep -qE "$expected_top"; then
         echo "   core $core: top frame '$top' does not match '$expected_top'" >&2
         return 1
      fi
   done
   return 0
}

unrelocated_path() {
   sed -E 's#^.*/spindle\.[0-9a-f]+##; s#/[0-9]+-spindlens-dso-#/#'
}

read_crash_site() {
   local core="$1"
   local binary="${2:-$TESTDIR/crash_test}"
   local map base cachepath real
   # Get the mappings from the coredump and extract the Spindle audit library
   map=$(gdb -batch -nx -ex 'set debuginfod enabled off' \
            -ex 'info proc mappings' "$binary" "$core" 2>/dev/null \
         | awk '/-spindlens-dso-libspindle_audit/ && $4 == "0x0" {print; exit}')
   base=$(printf '%s' "$map" | awk '{print $1}')
   cachepath=$(printf '%s' "$map" | awk '{print $NF}')
   # Figure out the path to the Spindle audit library
   real=$(printf '%s' "$cachepath" | unrelocated_path)
   # Open the coredump with the Spindle audit library symbols loaded
   # so we can check crash_site_buf
   gdb -batch -nx -ex 'set debuginfod enabled off' \
      -ex "add-symbol-file $real -o $base" \
      -ex 'printf "CRASH_SITE=%s\n", crash_site_buf' \
      "$binary" "$core" 2>/dev/null \
      | sed -n 's/^CRASH_SITE=//p' | head -1
}

# ---------------- crash log parsing ----------------

# The crash log is CSV with the fields:
#  - rank
#  - hostname
#  - pid
#  - timestamp
#  - exe
#  - site
#  - exemplar
#  - corepath

# Check that a crash log exists and starts with the expected header.
log_check_header() {
   local log="$1" header
   if [ ! -f "$log" ]; then
      echo "   missing crash log $log" >&2
      return 1
   fi
   IFS= read -r header <"$log"

   if [[ "$header" != "# "* ]]; then
      echo "   incorrect crash log header '$header'" >&2
      return 1
   fi
   return 0
}

log_rows() { grep -v '^#' $1 | grep -v '^$' | tail -n +1 ; }

# Split a log entry into ROW_RANK, ROW_HOST, ROW_PID, ROW_TS, ROW_EXE,
# ROW_SITE, ROW_EXEMPLAR, ROW_COREPATH.  The site and corepath fields may
# be CSV-quoted.
parse_log_row() {
   local rest
   IFS=, read -r ROW_RANK ROW_HOST ROW_PID ROW_TS rest <<<"$1"
   ROW_EXE="${rest%%,*}"
   rest="${rest#*,}"
   # Remove quoting if present
   if [[ "$rest" =~ ^\"(([^\"]|\"\")*)\"(,(.*))?$ ]]; then
      ROW_SITE="${BASH_REMATCH[1]//\"\"/\"}"
      rest="${BASH_REMATCH[4]}"
   else
      ROW_SITE="${rest%%,*}"
      rest="${rest#"$ROW_SITE"}"
      rest="${rest#,}"
   fi
   ROW_EXEMPLAR="${rest%%,*}"
   rest="${rest#*,}"
   if [[ "$rest" =~ ^\"(([^\"]|\"\")*)\"$ ]]; then
      ROW_COREPATH="${BASH_REMATCH[1]//\"\"/\"}"
   else
      ROW_COREPATH="$rest"
   fi
}

# The path the crash log should name for a test binary
original_path() {
   realpath "$TESTDIR/$1" 2>/dev/null || echo "$TESTDIR/$1"
}

# Check that the log names original paths, not Spindle's relocated copies
check_original_paths() {
   local expected_exe="$1" exe="$2" site="$3" object rc=0
   if [ "$exe" != "$expected_exe" ]; then
      echo "   exe in log was '$exe' but expected '$expected_exe'" >&2
      rc=1
   fi
   if [[ "$exe" == *-spindlens-* || "$site" == *-spindlens-* ]]; then
      echo "   exec in log incorrectly names a relocated file (exe '$exe', site '$site')" >&2
      rc=1
   fi
   if [[ "$site" != abort:* && "$site" == *+0x* ]]; then
      object="${site%+0x*}"
      if [ "${object##*/}" = "${expected_exe##*/}" ] && [ "$object" != "$expected_exe" ]; then
         echo "   site '$site' has incorrect exe name; expected '$expected_exe'" >&2
         rc=1
      fi
   fi
   return $rc
}

# ---------------- crash log verification ----------------

# Verify the crash log's rows against the mode's
# expectations and the coredumps on disk.
verify_crash_log() {
   local mode="$1"
   local dir="$2"
   local expected_sites="$3"
   local log="$dir/crash.log"
   local expected_total
   expected_total=$(resolve_crashers "$mode")

   local expected_site="${TEST_SITE[$mode]:-}"
   [ "$expected_site" = "-" ] && expected_site=""

   local binary_name="${TEST_BINARY[$mode]:-crash_test}"
   local expected_exe
   expected_exe=$(original_path "$binary_name")

   log_check_header "$log" || return 1

   # Fork-child modes log rows from children of a rank
   local forkchild=0 rank_limit="$NODES" ident
   if has_flag "$mode" forkchild; then
      forkchild=1
      rank_limit=$(( 2 * NODES ))
   fi

   local rc=0 total=0 line key
   local -A seen=() site_exemplar=() site_corepath=() site_exemplar_pid=()
   while IFS= read -r line; do
      if [[ "$line" == "#"* ]]; then
          continue
      fi
      if [[ "$line" == "" ]]; then
          continue
      fi
      parse_log_row "$line"
      key="$ROW_EXE|$ROW_SITE"
      total=$((total + 1))
      if ! [[ "$ROW_RANK" =~ ^[0-9]+$ ]] || [ "$ROW_RANK" -ge "$rank_limit" ]; then
         echo "   rank '$ROW_RANK' outside expected range [0,$rank_limit)" >&2
         rc=1
         continue
      fi
      if [ -z "$ROW_HOST" ]; then
         echo "   rank $ROW_RANK: empty hostname" >&2
         rc=1
      fi
      if ! [[ "$ROW_TS" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}[+-][0-9]{4}$ ]]; then
         echo "   rank $ROW_RANK: timestamp '$ROW_TS' is not of the form YYYY-MM-DDTHH:MM:SS+ZZZZ" >&2
         rc=1
      fi
      if ! [[ "$ROW_PID" =~ ^[1-9][0-9]*$ ]]; then
         echo "   rank $ROW_RANK: pid '$ROW_PID' is not a positive integer" >&2
         rc=1
      fi
      ident="$ROW_RANK"
      [ "$forkchild" = "1" ] && ident="$ROW_RANK/$ROW_PID"
      if [ -n "${seen[$ident]:-}" ]; then
         echo "   rank/pid $ident repeated" >&2
         rc=1
      fi
      seen[$ident]=1

      if [ -z "${site_exemplar[$key]:-}" ]; then
         site_exemplar[$key]="$ROW_EXEMPLAR"
         site_corepath[$key]="$ROW_COREPATH"
         check_original_paths "$expected_exe" "$ROW_EXE" "$ROW_SITE" || rc=1
         if [ -n "$expected_site" ] && ! [[ "$ROW_SITE" =~ $expected_site ]]; then
            echo "   site '$ROW_SITE' does not match '$expected_site'" >&2
            rc=1
         fi
         if [ -z "$ROW_COREPATH" ]; then
            echo "   site '$key': empty corepath" >&2
            rc=1
         fi
      else
         if [ "${site_exemplar[$key]}" != "$ROW_EXEMPLAR" ]; then
            echo "   site '$key' exemplar ${site_exemplar[$key]} does not match expected $ROW_EXEMPLAR" >&2
            rc=1
         fi
         if [ "${site_corepath[$key]}" != "$ROW_COREPATH" ]; then
            echo "   site '$key' has differing corepaths '${site_corepath[$key]}' and '$ROW_COREPATH'" >&2
            rc=1
         fi
      fi
      # The exemplar's own row
      if [ "$ROW_RANK" = "$ROW_EXEMPLAR" ] &&
         [[ "${ROW_COREPATH##*/}" =~ (^|[^0-9])$ROW_PID([^0-9]|$) ]]; then
         if [ -n "${site_exemplar_pid[$key]:-}" ]; then
            echo "   site '$key': exemplar rank $ROW_EXEMPLAR pids ${site_exemplar_pid[$key]} and $ROW_PID both match corepath '$ROW_COREPATH'" >&2
            rc=1
         fi
         site_exemplar_pid[$key]="$ROW_PID"
      fi
   done < <(log_rows "$log")

   # Each site's exemplar row must exist and its core must have been written
   local cores
   cores=$(core_files "$dir")
   for key in "${!site_exemplar[@]}"; do
      if [ -z "${site_exemplar_pid[$key]:-}" ]; then
         echo "   site '$key': no row of exemplar rank ${site_exemplar[$key]} has its pid in corepath '${site_corepath[$key]}'" >&2
         rc=1
      elif ! grep -qxF -- "${site_corepath[$key]}" <<<"$cores"; then
         echo "   site '$key': logged corepath '${site_corepath[$key]}' (pid ${site_exemplar_pid[$key]}) was not written" >&2
         rc=1
      fi
   done

   if [ "${#site_exemplar[@]}" != "$expected_sites" ]; then
      echo "   ${#site_exemplar[@]} crashsites, expected $expected_sites" >&2
      rc=1
   fi
   if [ "$total" != "$expected_total" ]; then
      echo "   $total total ranks, expected $expected_total" >&2
      rc=1
   fi
   return $rc
}

# Verify that logged crash site matches what is recorded in the coredump.
verify_log_matches_core() {
   local mode="$1"
   local dir="$2"
   local log="$dir/crash.log"
   local binary="$TESTDIR/${TEST_BINARY[$mode]:-crash_test}"

   local core core_site
   core=$(core_files "$dir" | head -1)
   [ -n "$core" ] || { echo "   no core for gdb cross-check" >&2; return 1; }
   parse_log_row "$(log_rows "$log" | head -1)"
   core_site=$(read_crash_site "$core" "$binary")
   if [ -z "$core_site" ]; then
      echo "   could not read crash site from $core" >&2
      return 1
   fi
   core_site=$(printf '%s' "$core_site" | unrelocated_path)
   if [ "$core_site" != "$ROW_SITE" ]; then
      echo "   core site '$core_site' != logged site '$ROW_SITE'" >&2
      return 1
   fi
   return 0
}

# ---------------- session tests ----------------

# Common setup for the session-based tests.
#   SESSION_DIR      per-test scratch directory
#   SESSION_LOG      crash log path inside SESSION_DIR
#   SESSION_RUN      launcher command to run a program on every node
#                    inside the session
#   SESSION_RUN_ONE  launcher command to run on one node within session
session_test_setup() {
   local name="$1" flux_run
   case "$LAUNCHER" in
      slurm-plugin)
         SESSION_RUN="srun --spindle"
         SESSION_RUN_ONE="srun --spindle -N1 -n1"
         ;;
      flux)
         flux_run="flux run -o userrc=$SPINDLE_RC -o spindle --env=LD_LIBRARY_PATH"
         SESSION_RUN="$flux_run -N$NODES -n$NODES --"
         SESSION_RUN_ONE="$flux_run -N1 -n1 --"
         ;;
      *) die "--$name requires --launcher=slurm-plugin or --launcher=flux" ;;
   esac

   mkdir -p "$CRASH_TEST_SCRATCH" || die "can't create scratch dir '$CRASH_TEST_SCRATCH'"
   SESSION_DIR=$(mktemp -d "$CRASH_TEST_SCRATCH/$name.XXXXXX") || die "can't create test dir"
   SESSION_LOG="$SESSION_DIR/crash.log"
}

collect_log_sites() {
   local log="$1" line key
   LOG_SITES=()
   LOG_TOTAL=0
   log_check_header "$log" || return 0
   while IFS= read -r line; do
      parse_log_row "$line"
      key="$ROW_EXE|$ROW_SITE"
      LOG_SITES[$key]=$(( ${LOG_SITES[$key]:-0} + 1 ))
      LOG_TOTAL=$((LOG_TOTAL + 1))
   done < <(log_rows "$log")
}

session_test_launch() {
   local session_opts="--crash-dedup --crash-log=$SESSION_LOG${1:+ $1}"
   ulimit -c unlimited
   # Make libcrashfuncs.so visible to dlopen() from the session's scratch dir.
   export LD_LIBRARY_PATH="$TESTDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
   case "$LAUNCHER" in
      slurm-plugin)
         ( cd "$SESSION_DIR" && salloc -N"$NODES" -n"$NODES" \
              --spindle-session="$session_opts" bash "$SESSION_DIR/inner.sh" ) \
            >"$SESSION_DIR/stdout.log" 2>"$SESSION_DIR/stderr.log"
         ;;
      flux)
         # The session's crash options must be given at session start
         local sid
         sid=$("$SPINDLE" --start-session $session_opts 2>"$SESSION_DIR/session.log") || \
            die "spindle --start-session failed (see $SESSION_DIR/session.log)"
         ( cd "$SESSION_DIR" && bash "$SESSION_DIR/inner.sh" ) \
            >"$SESSION_DIR/stdout.log" 2>"$SESSION_DIR/stderr.log"
         "$SPINDLE" "--end-session${sid:+=$sid}" >>"$SESSION_DIR/session.log" 2>&1
         ;;
   esac
   # Wait briefly for the server to shut down and write the log
   sleep 2
}

# Session-mode crash-log test
# two crashing runs inside one spindle session share a crash log.
run_session_test() {
   session_test_setup session
   local dir="$SESSION_DIR" log="$SESSION_LOG"

   cat >"$dir/inner.sh" <<EOF
$SESSION_RUN "$TESTDIR/crash_test" --crash-mode all-same
sleep 3
if [ -e "$log" ]; then echo present; else echo absent; fi > "$dir/log_after_run1"
$SESSION_RUN "$TESTDIR/crash_test" --crash-mode sigabrt
sleep 3
if [ -e "$log" ]; then echo present; else echo absent; fi > "$dir/log_after_run2"
EOF
   session_test_launch

   local after1 after2 ok=1
   after1=$(cat "$dir/log_after_run1" 2>/dev/null || echo missing)
   after2=$(cat "$dir/log_after_run2" 2>/dev/null || echo missing)
   collect_log_sites "$log"

   [ "$after1" = "absent" ] || { echo "FAIL session: log $after1 after run 1"; ok=0; }
   [ "$after2" = "absent" ] || { echo "FAIL session: log $after2 after run 2"; ok=0; }
   [ "${#LOG_SITES[@]}" = "2" ] || { echo "FAIL session: ${#LOG_SITES[@]} sites after session end"; ok=0; }
   [ "$LOG_TOTAL" = "$((2 * NODES))" ] || \
      { echo "FAIL session: $LOG_TOTAL total ranks in final log (expected $((2 * NODES)))"; ok=0; }

   [ "$ok" = "1" ] || exit 1
   echo "PASS session"
}

# Cross-executable dedup test: two different executables crashing
# at the same offset in the same shared library inside one session
# should not be deduplicated
run_cross_exe_test() {
   session_test_setup cross-exe
   local dir="$SESSION_DIR"

   cat >"$dir/inner.sh" <<EOF
$SESSION_RUN "$TESTDIR/crash_test" --crash-mode in-library
sleep 3
$SESSION_RUN "$TESTDIR/crash_test_pie" --crash-mode in-library
sleep 3
EOF
   session_test_launch

   local plain pie ncores key exe site prev_site="" ok=1
   plain=$(original_path crash_test)
   pie=$(original_path crash_test_pie)
   ncores=$(count_cores "$dir")
   collect_log_sites "$dir/crash.log"

   [ "$ncores" = "2" ] || { echo "FAIL cross-exe: $ncores coredumps (expected 2)"; ok=0; }
   [ "${#LOG_SITES[@]}" = "2" ] || { echo "FAIL cross-exe: ${#LOG_SITES[@]} crashsites (expected 2)"; ok=0; }

   # Both programs crashed at the same libcrashfuncs.so site on every node,
   # so the two keys differ only in their executable
   for key in "${!LOG_SITES[@]}"; do
      exe="${key%%|*}"
      site="${key#*|}"
      if [ "$exe" != "$plain" ] && [ "$exe" != "$pie" ]; then
         echo "FAIL cross-exe: unexpected executable '$exe'"
         ok=0
      fi
      if ! [[ "$site" =~ libcrashfuncs\.so\+0x ]]; then
         echo "FAIL cross-exe: crash site '$site' is not in libcrashfuncs.so"
         ok=0
      elif [ -n "$prev_site" ] && [ "$site" != "$prev_site" ]; then
         echo "FAIL cross-exe: crash sites unexpectedly differ ('$prev_site' vs '$site')"
         ok=0
      fi
      prev_site="$site"
      [ "${LOG_SITES[$key]}" = "$NODES" ] || \
         { echo "FAIL cross-exe: site '$key' has ${LOG_SITES[$key]} rows (expected $NODES)"; ok=0; }
   done

   [ "$ok" = "1" ] || exit 1
   echo "PASS cross-exe"
}

# Original path test
# Verifies that if a given executable is relocated to different
# names on different nodes, these are still treated as the same site
# for deduplication purposes and are recorded in the log under
# the original path.
run_orig_path_test() {
   [ "$NODES" -ge 3 ] || die "--orig-path requires at least 3 nodes"
   session_test_setup orig-path
   local dir="$SESSION_DIR" log="$SESSION_LOG"

   cat >"$dir/inner.sh" <<EOF
$SESSION_RUN_ONE "$TESTDIR/crash_test_pie" --crash-mode safepoint
sleep 3
$SESSION_RUN "$TESTDIR/crash_test" --crash-mode all-same
sleep 3
EOF
   session_test_launch --pull

   local expected_exe ncores key ok=1
   expected_exe=$(original_path crash_test)
   ncores=$(count_cores "$dir")
   collect_log_sites "$log"
   for key in "${!LOG_SITES[@]}"; do
      check_original_paths "$expected_exe" "${key%%|*}" "${key#*|}" || ok=0
   done

   [ "$ncores" = "1" ] || { echo "FAIL orig-path: $ncores coredumps (expected 1)"; ok=0; }
   [ "${#LOG_SITES[@]}" = "1" ] || { echo "FAIL orig-path: ${#LOG_SITES[@]} crashsites (expected 1)"; ok=0; }
   [ "$LOG_TOTAL" = "$NODES" ] || { echo "FAIL orig-path: $LOG_TOTAL rows (expected $NODES)"; ok=0; }

   [ "$ok" = "1" ] || exit 1
   echo "PASS orig-path"
}

# ---------------- test verification helpers ----------------

# A "clean" mode must exit 0 with no coredumps and no crash log.
verify_clean_mode() {
   local mode="$1" dir="$2" launch_rc="$3"
   local actual_cores
   actual_cores=$(count_cores "$dir")
   if [ "$actual_cores" != "0" ]; then
      echo "FAIL $mode: expected 0 dumps, got $actual_cores"
      return 1
   fi
   if [ "$launch_rc" != "0" ]; then
      echo "FAIL $mode: launcher exited $launch_rc (expected 0)"
      return 1
   fi
   # No crashes means no crash log
   if [ -e "$dir/crash.log" ]; then
      echo "FAIL $mode: crash log unexpectedly written"
      return 1
   fi
   echo "PASS $mode (clean exit)"
}

# A crashing mode must produce the expected coredumps and a crash log
# that agrees with them.
verify_crashed_mode() {
   local mode="$1" dir="$2"
   local want actual
   want=$(resolve_cores "$mode")
   actual=$(count_cores "$dir")

   if [ "$actual" != "$want" ]; then
      echo "FAIL $mode: expected $want coredumps, got $actual"
      return 1
   fi

   # Verify that the core files show the expected crash sites
   if ! verify_top_frames "$mode" "$dir"; then
      echo "FAIL $mode: coredump top-frame verification failed"
      return 1
   fi

   # Verify sites, ranks, counts, and exemplars from the crash log, and
   # that each exemplar's logged core was written
   if ! verify_crash_log "$mode" "$dir" "$actual"; then
      echo "FAIL $mode: crash log verification failed"
      return 1
   fi

   # Check the coredump's recorded crash site against the log
   if [ "$LAUNCHER" = "serial" ] && [ "$mode" = "all-same" ]; then
      if ! verify_log_matches_core "$mode" "$dir"; then
         echo "FAIL $mode: gdb check of crash site does not match log"
         return 1
      fi
   fi

   echo "PASS $mode ($actual dumps)"
}

# Run one sweep mode in its own scratch directory and verify the results.
# Prints the mode's PASS/FAIL line; returns 0 iff the mode passed.
run_one_mode() {
   local mode="$1"

   # Run each test in a per-test directory so all its coredumps land in
   # one place and we can count them
   mkdir -p "$CRASH_TEST_SCRATCH" || die "can't create scratch dir '$CRASH_TEST_SCRATCH'"
   local dir
   dir=$(mktemp -d "$CRASH_TEST_SCRATCH/$mode.XXXXXX") || die "can't create test dir under '$CRASH_TEST_SCRATCH'"
   local launch_rc=0
   ( cd "$dir" && launch "$mode" "$dir/crash.log" ) >"$dir/stdout.log" 2>"$dir/stderr.log" || launch_rc=$?
   [ "$LAUNCHER" = "serial" ] || sleep 1

   if has_flag "$mode" clean; then
      verify_clean_mode "$mode" "$dir" "$launch_rc"
   else
      verify_crashed_mode "$mode" "$dir"
   fi
}

main() {
   parse_table
   parse_args "$@"

   if [ -z "$CRASH_TEST_SCRATCH" ]; then
      if [ -n "${SPINDLE_TEST_CONTAINER:-}" ]; then
         die "--scratch=DIR is required when running in a CI container"
      fi
      CRASH_TEST_SCRATCH="$TESTDIR/spindle_crash_test"
   fi
   check_prereqs

   if [ -n "$SESSION_TEST" ]; then
      "run_${SESSION_TEST//-/_}_test"
      return
   fi

   local pass=0 fail=0

   local modes_to_run
   if [ -z "$MODES" ]; then
      modes_to_run=("${DEFAULT_MODES[@]}")
   else
      modes_to_run=()
      IFS=',' read -ra specified_modes <<< "$MODES"
      for m in "${specified_modes[@]}"; do
         modes_to_run+=("$(trim "$m")")
      done
   fi

   for mode in "${modes_to_run[@]}"; do
      if [ -z "${TEST_CORES[$mode]+set}" ]; then
         die "unknown mode '$mode'"
      fi

      if has_flag "$mode" multi-rank && \
         { [ "$LAUNCHER" = "serial" ] || [ "$NODES" -lt 2 ]; }; then
         echo "SKIP $mode (needs multiple ranks)"
         continue
      fi

      if run_one_mode "$mode"; then
         pass=$((pass+1))
      else
         fail=$((fail+1))
      fi
   done

   echo
   echo "Summary: $pass passed, $fail failed"
   [ "$fail" -eq 0 ] || exit 1
}

main "$@"
