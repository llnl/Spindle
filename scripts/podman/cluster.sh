#!/bin/bash
#
# cluster.sh - bring a Slurm test cluster up and down under rootless podman.
# Sourced by the test scripts; defines functions only.  Needs: podman, jq, openssl.
#
# The cluster for a prefix P (default 4 workers) is, on network P-net:
#     P-mariadb   P-db (slurmdbd)   P-head (slurmctld)   P-node-1 .. P-node-N (slurmd)
# Each container's name equals its hostname equals the name slurm.conf uses, so ordinary
# DNS names work, and several clusters (different prefixes) can coexist.
#
# Why this starts aardvark-dns itself (found with scripts/podman/probe-dns.sh, 13Nov2026):
#   netavark launches aardvark-dns through `systemd-run --user --scope`, which fails when the
#   user has no systemd session (no /run/user/<uid>).  netavark ignores the failure and no DNS
#   server runs.  Started by hand inside the rootless netns it works, and it picks up
#   containers started afterwards.  If name resolution still fails, cluster_up falls back to
#   /etc/hosts entries read from aardvark's own data file.
#
# Usage:
#     cluster_up PREFIX IMAGE     0 when slurm is up and every worker is idle
#     cluster_run_tests PREFIX    runs the testsuite under salloc; returns its status
#     cluster_down PREFIX         removes containers and network
#     cluster_snapshot PREFIX [DIR] [REASON]   evidence from a cluster (see below)
#     cluster_stop_aardvark       stops the shared DNS server (cleanup and defenestrate do this)
#
# Environment:
#     CLUSTER_WORKERS=N     worker count (default 4)
#     CLUSTER_KEEP=1        leave the cluster running for inspection (cluster_down does nothing)
#     CLUSTER_MARIADB_IMAGE image for the database (default: T_PULL[mariadb] in targets.sh)
#     CLUSTER_RUN_ARGS="..." extra `podman run` options for the Slurm containers (not MariaDB),
#                           e.g. uid maps; test-spindle-slurm.sh fills it from T_RUN_ARGS
#     CLUSTER_TEST_TIMEOUT=N seconds without a new test starting before a run is declared hung
#                           (default 600; 0 disables).  A snapshot is then taken and the run aborted.

CLUSTER_WORKERS="${CLUSTER_WORKERS:-4}"
CLUSTER_KEEP="${CLUSTER_KEEP:-0}"
# The database image comes from the target registry, so its version is defined in one place.
# shellcheck source=scripts/podman/targets.sh
source "$(dirname "${BASH_SOURCE[0]}")/targets.sh"
CLUSTER_MARIADB_IMAGE="${CLUSTER_MARIADB_IMAGE:-${T_PULL[mariadb]}}"
CLUSTER_TEST_TIMEOUT="${CLUSTER_TEST_TIMEOUT:-600}"

CL_PREFIX=""
CL_IMAGE=""
CL_NET=""
CL_USE_HOSTS=0
CL_ADDHOST=()
CL_T0=$SECONDS

cl_log()  { printf '[%s +%3ds] %s\n' "${CL_PREFIX:-cluster}" $((SECONDS - CL_T0)) "$*"; }
cl_warn() { cl_log "WARNING: $*"; }
cl_err()  { cl_log "ERROR: $*" >&2; }

# Container names for a prefix, one per line.
cl_names() {
    local p=$1 i
    for i in $(seq 1 "$CLUSTER_WORKERS"); do printf '%s-node-%s\n' "$p" "$i"; done
    printf '%s\n' "$p-mariadb" "$p-db" "$p-head"
}

#------------------------------------------------------------------------------
# aardvark-dns
#------------------------------------------------------------------------------

cl_aardvark_bin() {
    podman info --format json 2>/dev/null | jq -r '.host.networkBackendInfo.dns.path // empty'
}

cl_aardvark_dir() {
    printf '%s/networks/aardvark-dns\n' "$(podman info --format '{{.Store.RunRoot}}' 2>/dev/null)"
}

cl_aardvark_running() {
    pgrep -u "$(id -u)" -x aardvark-dns >/dev/null 2>&1
}

# Starts the server unless one is already running.  It daemonizes, so poll for the process.
# The bridge's gateway address must already exist (it appears with the network's first container).
cl_aardvark_start() {
    cl_aardvark_running && return 0
    local bin dir _
    bin=$(cl_aardvark_bin)
    dir=$(cl_aardvark_dir)
    [[ -x "$bin" ]] || { cl_warn "aardvark-dns binary not found ('$bin')"; return 1; }
    [[ -d "$dir" ]] || { cl_warn "aardvark config directory $dir does not exist"; return 1; }
    timeout 20 podman unshare --rootless-netns "$bin" --config "$dir" -p 53 run >/dev/null 2>&1
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        sleep 0.5
        cl_aardvark_running && return 0
    done
    return 1
}

cluster_stop_aardvark() {
    pkill -u "$(id -u)" -x aardvark-dns 2>/dev/null
    return 0
}

#------------------------------------------------------------------------------
# Name resolution checks and the /etc/hosts fallback
#------------------------------------------------------------------------------

# cl_resolves FROM-CONTAINER NAME [SECONDS]
cl_resolves() {
    local from=$1 name=$2 end=$((SECONDS + ${3:-10}))
    while :; do
        podman exec "$from" getent hosts "$name" >/dev/null 2>&1 && return 0
        (( SECONDS >= end )) && return 1
        sleep 0.5
    done
}

# Lines of "IP NAME" for every container registered on this cluster's network, from aardvark's
# data file (line 1 is the gateway; each later line is "ID IP name,alias,...").
cl_peer_table() {
    awk 'NR > 1 && NF >= 3 { split($3, a, ","); print $2, a[1] }' "$(cl_aardvark_dir)/$CL_NET" 2>/dev/null
}

# Fills CL_ADDHOST with --add-host options for every container registered so far.
cl_refresh_addhost() {
    CL_ADDHOST=()
    (( CL_USE_HOSTS )) || return 0
    local ip name
    while read -r ip name; do
        [[ -n "$name" ]] && CL_ADDHOST+=(--add-host "$name:$ip")
    done < <(cl_peer_table)
}

# cl_inject_hosts CONTAINER...   append the whole peer table to each container's /etc/hosts
cl_inject_hosts() {
    local table c
    table=$(cl_peer_table)
    for c in "$@"; do
        printf '%s\n' "$table" | podman exec --user root -i "$c" tee -a /etc/hosts >/dev/null
    done
}

# cl_ensure_dns FROM-CONTAINER NAME...   switches to hosts mode if any name does not resolve
cl_ensure_dns() {
    (( CL_USE_HOSTS )) && return 0
    local from=$1 n
    shift
    for n in "$@"; do
        if ! cl_resolves "$from" "$n" 10; then
            cl_warn "$from cannot resolve $n: using /etc/hosts entries instead of DNS"
            CL_USE_HOSTS=1
            return 0
        fi
    done
    cl_log "name resolution works (checked $# names from $from)"
}

#------------------------------------------------------------------------------
# Readiness
#------------------------------------------------------------------------------

# cl_wait DESCRIPTION SECONDS COMMAND...
cl_wait() {
    local what=$1 limit=$2 t0=$SECONDS
    shift 2
    while :; do
        if "$@" >/dev/null 2>&1; then
            cl_log "$what ready ($((SECONDS - t0))s)"
            return 0
        fi
        if (( SECONDS - t0 >= limit )); then
            cl_err "$what not ready after ${limit}s"
            return 1
        fi
        sleep 1
    done
}

cl_tcp()      { podman exec "$1" timeout 3 bash -c "exec 3<>/dev/tcp/127.0.0.1/$2"; }
cl_sinfo_ok() { podman exec "$1-head" sinfo; }
cl_workers_idle() {
    local n
    n=$(podman exec "$1-head" sinfo -h -N -o '%T' 2>/dev/null | grep -cx idle)
    [[ "$n" -ge "$CLUSTER_WORKERS" ]]
}

cl_diagnose() {
    local p=$1 c
    cl_log "---------- diagnostics ----------"
    podman ps -a --filter "name=$p-" --format 'table {{.Names}}\t{{.Status}}' 2>&1
    echo "DNS server running: $(cl_aardvark_running && echo yes || echo no)   hosts mode: $CL_USE_HOSTS"
    for c in $(cl_names "$p"); do
        podman container exists "$c" 2>/dev/null || continue
        echo "----- last 25 lines of $c"
        podman logs --tail 25 "$c" 2>&1 | grep -v 'GENERATED ROOT PASSWORD'
    done
    cl_log "---------------------------------"
}

#------------------------------------------------------------------------------
# Containers
#------------------------------------------------------------------------------

# cl_run_slurm ROLE NAME [extra podman run options]
cl_run_slurm() {
    local role=$1 name=$2
    shift 2
    cl_refresh_addhost
    # shellcheck disable=SC2086  # CLUSTER_RUN_ARGS is a list of options, intentionally word-split
    podman run -d --name "$name" --hostname "$name" --network "$CL_NET" \
        ${CL_ADDHOST[@]+"${CL_ADDHOST[@]}"} \
        ${CLUSTER_RUN_ARGS-} \
        -e SLURM_ROLE="$role" \
        -e SLURM_HEAD_NODE="$CL_PREFIX-head" \
        -e SLURM_DB_HOST="$CL_PREFIX-db" \
        -e SLURM_MARIADB_HOST="$CL_PREFIX-mariadb" \
        -e SLURM_NODE_PREFIX="$CL_PREFIX-node" \
        -e workers="$CLUSTER_WORKERS" \
        "$@" "$CL_IMAGE" >/dev/null
}

#------------------------------------------------------------------------------
# Public entry points
#------------------------------------------------------------------------------

cluster_down() {
    local p=${1:-$CL_PREFIX} names=()
    if [[ "$CLUSTER_KEEP" == 1 ]]; then
        echo "CLUSTER_KEEP=1: leaving cluster '$p' running.  Inspect with:"
        echo "  podman exec -it $p-head bash"
        echo "Remove with:"
        echo "  podman rm -f --time 0 \$(podman ps -a --format '{{.Names}}' | grep '^$p-') ; podman network rm -f $p-net"
        return 0
    fi
    pkill -u "$(id -u)" -f "podman exec .*$p-head" 2>/dev/null
    mapfile -t names < <(cl_names "$p")
    podman rm --force --ignore --time 0 "${names[@]}" >/dev/null 2>&1
    podman network rm --force "$p-net" >/dev/null 2>&1
    # One aardvark-dns serves every cluster, so stop it only when no container of yours is left.
    if [[ -z "$(podman ps -q 2>/dev/null)" ]]; then
        cluster_stop_aardvark
    fi
    return 0
}

cluster_up() {
    local p=$1 image=$2 i pw pid rc
    local nodes=() pids=() all=()
    CL_PREFIX=$p
    CL_IMAGE=$image
    CL_NET="$p-net"
    CL_USE_HOSTS=0
    CL_T0=$SECONDS
    for i in $(seq 1 "$CLUSTER_WORKERS"); do nodes+=("$p-node-$i"); done
    all=("${nodes[@]}" "$p-db" "$p-head")

    local keep=$CLUSTER_KEEP
    CLUSTER_KEEP=0
    cluster_down "$p"            # clear leftovers from an earlier run
    CLUSTER_KEEP=$keep

    pw=$(openssl rand -hex 16)

    cl_log "creating network $CL_NET"
    podman network create "$CL_NET" >/dev/null || { cl_err "network create failed"; return 1; }

    cl_log "starting MariaDB"
    if ! podman run -d --name "$p-mariadb" --hostname "$p-mariadb" --network "$CL_NET" \
            -e MYSQL_RANDOM_ROOT_PASSWORD=yes -e MYSQL_DATABASE=slurm_acct_db \
            -e MYSQL_USER=slurm -e MYSQL_PASSWORD="$pw" "$CLUSTER_MARIADB_IMAGE" >/dev/null; then
        cl_err "could not start MariaDB (is $CLUSTER_MARIADB_IMAGE available on this node?)"
        return 1
    fi

    # The DNS server binds the bridge's gateway address, which exists once a container has joined.
    if cl_aardvark_start; then
        cl_log "aardvark-dns running"
    else
        cl_warn "could not start aardvark-dns: using /etc/hosts entries"
        CL_USE_HOSTS=1
    fi

    cl_log "starting $CLUSTER_WORKERS workers"
    for i in $(seq 1 "$CLUSTER_WORKERS"); do
        cl_run_slurm worker "$p-node-$i" &
        pids+=($!)
    done
    rc=0
    for pid in "${pids[@]}"; do wait "$pid" || rc=1; done
    if (( rc != 0 )); then cl_err "a worker failed to start"; cl_diagnose "$p"; return 1; fi

    cl_wait "MariaDB (port 3306)" 120 cl_tcp "$p-mariadb" 3306 || { cl_diagnose "$p"; return 1; }
    cl_ensure_dns "$p-node-1" "$p-mariadb" "${nodes[@]}"

    cl_log "starting slurmdbd"
    cl_run_slurm db "$p-db" -e MARIADB_PASSWORD="$pw" || { cl_err "slurmdbd failed to start"; cl_diagnose "$p"; return 1; }
    cl_wait "slurmdbd (port 6819)" 90 cl_tcp "$p-db" 6819 || { cl_diagnose "$p"; return 1; }

    cl_log "starting slurmctld"
    cl_run_slurm ctl "$p-head" -t || { cl_err "slurmctld failed to start"; cl_diagnose "$p"; return 1; }

    cl_ensure_dns "$p-node-1" "$p-head" "$p-db"
    if (( CL_USE_HOSTS )); then
        cl_log "writing /etc/hosts entries"
        cl_inject_hosts "${all[@]}"
    fi

    cl_wait "slurmctld (sinfo)" 90 cl_sinfo_ok "$p" || { cl_diagnose "$p"; return 1; }
    cl_wait "all $CLUSTER_WORKERS workers idle" 120 cl_workers_idle "$p" || { cl_diagnose "$p"; return 1; }

    podman exec "$p-head" bash -c 'munge -n | unmunge' >/dev/null 2>&1 \
        || { cl_err "munge check failed in $p-head"; cl_diagnose "$p"; return 1; }

    cl_log "cluster ready (dns mode: $( (( CL_USE_HOSTS )) && echo /etc/hosts || echo aardvark-dns ))"
    return 0
}

#------------------------------------------------------------------------------
# Snapshots: evidence for hangs and failures
#------------------------------------------------------------------------------

CL_REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CL_EXEC_ERR=""

# `spindle-podman test` sets SPINDLE_PODMAN_LOG_DIR to the run's log directory.
cl_logdir() { printf '%s\n' "${SPINDLE_PODMAN_LOG_DIR:-$CL_REPO_ROOT/.podman-logs}"; }

# Masks passwords in whatever flows through.  The generated passwords are throwaway, but
# snapshots are kept on shared storage.
cl_redact() {
    sed -E -e 's/([A-Za-z_]*PASSWORD)=[^",\\[:space:]]*/\1=REDACTED/g' \
           -e 's/(GENERATED ROOT PASSWORD:).*/\1 REDACTED/' \
           -e 's/(StoragePass=).*/\1REDACTED/'
}

cl_hdr() { printf '\n######## %s\n' "$*"; }

# Everything that enters a container runs under timeout: exec into a wedged container can hang.
cl_px()   { local c=$1; shift; timeout 30 podman exec "$c" "$@" 2>&1; }
cl_pxsh() { local c=$1; shift; timeout 30 podman exec "$c" bash -c "$*" 2>&1; }

# The images may lack `ps`, so processes are read from /proc: pid, state, ppid, kernel wait
# channel, command line.  These strings run inside the container, so they are single-quoted.
# shellcheck disable=SC2016
CL_PROCS='printf "%6s %-6s %-22s %s\n" PID "ST PPID" WCHAN COMMAND
for d in /proc/[0-9]*; do
  p=${d#/proc/}
  s=$(sed -E "s/^[0-9]+ \((.*)\) (.) ([0-9]+) .*/\2 \3/" $d/stat 2>/dev/null) || continue
  printf "%6s %-6s %-22s %s\n" "$p" "$s" "$(cat $d/wchan 2>/dev/null)" "$(tr "\0" " " < $d/cmdline 2>/dev/null | cut -c1-150)"
done'

# Prints the first pid whose command name is $1 (as a command to run in the container).
# shellcheck disable=SC2016
cl_findpid_cmd() { printf 'for d in /proc/[0-9]*; do [ "$(cat $d/comm 2>/dev/null)" = "%s" ] && echo ${d#/proc/}; done | head -1' "$1"; }

# cl_backtrace CONTAINER COMMAND-NAME   (gdb attach, best effort)
cl_backtrace() {
    local c=$1 comm=$2 pid
    pid=$(cl_pxsh "$c" "$(cl_findpid_cmd "$comm")")
    if [[ ! "$pid" =~ ^[0-9]+$ ]]; then echo "(no process named $comm in $c)"; return 0; fi
    echo "$comm in $c is pid $pid"
    timeout 60 podman exec "$c" gdb -p "$pid" -batch -ex 'thread apply all bt 15' 2>&1 \
        | grep -v '^\[New LWP\|^warning:\|^Reading symbols\|^Using host libthread' | head -60
}

# cl_snapshot_report PREFIX REASON EXEC_OK   -- the text report, on stdout
cl_snapshot_report() {
    local p=$1 reason=$2 exec_ok=$3 c i
    local members=("$p-head")
    for i in $(seq 1 "$CLUSTER_WORKERS"); do members+=("$p-node-$i"); done

    echo "snapshot of cluster '$p' on $(hostname) at $(date)"
    echo "reason: $reason"
    echo "XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-<unset>}  CONTAINERS_CONF=${CONTAINERS_CONF:-<unset>}  SPINDLE_DEBUG=${SPINDLE_DEBUG:-<unset>}"

    cl_hdr "containers (podman's own database; needs no exec)"
    podman ps -a --filter "name=$p-" --format 'table {{.Names}}\t{{.Status}}' 2>&1
    echo "aardvark-dns pids: $(pgrep -u "$(id -u)" -x aardvark-dns | tr '\n' ' ')"

    cl_hdr "this user's container-related processes"
    # shellcheck disable=SC2009  # pgrep cannot print these columns
    ps -u "$(id -u)" -o pid,ppid,etime,stat,comm,args 2>/dev/null \
        | grep -E 'conmon|catatonit|aardvark|slirp4netns|crun|podman|munged|slurm' | grep -v 'grep -E' | cut -c1-200

    cl_hdr "runtime directory"
    echo "XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-<unset>}"
    find "${XDG_RUNTIME_DIR:-/nonexistent}" -maxdepth 3 -printf '%M %u %TY-%Tm-%Td %TH:%TM %p\n' 2>&1 | head -60
    echo "/run/user/$(id -u): $(stat -c '%A %U changed %z' "/run/user/$(id -u)" 2>&1)"

    if [[ "$exec_ok" != 1 ]]; then
        cl_hdr "podman exec does not work in this shell: nothing inside the containers was collected"
        printf '%s\n' "$CL_EXEC_ERR"
        echo "XDG_RUNTIME_DIR here: ${XDG_RUNTIME_DIR:-<unset>}.  Use the one that started the cluster:"
        echo "    eval \"\$(./spindle-podman env)\""
        return 0
    fi

    cl_hdr "how Spindle was built to launch its daemons (srun flavor: HAVE_SRUN_EXTERNAL_LAUNCHER must be 1 on Slurm >= 23.11)"
    # shellcheck disable=SC2016
    cl_pxsh "$p-head" 'srun --version; grep -rh "^#define HAVE_SRUN_EXTERNAL_LAUNCHER\|^#define BROKEN_SRUN\|^#define RSHLAUNCH_ENABLED\|^#define ENABLE_SLURM_PLUGIN" $(find "$HOME/Spindle-build" -name config.h) 2>&1 | sort | uniq -c'

    cl_hdr "slurm view (head)"
    cl_px "$p-head" squeue
    cl_px "$p-head" sinfo -N
    cl_pxsh "$p-head" 'scontrol show job 2>&1 | head -40'

    for c in "${members[@]}"; do
        cl_hdr "processes in $c"
        cl_pxsh "$c" "$CL_PROCS" | grep -v 'sed -E\|/proc/\[0-9\]'
        cl_hdr "tcp sockets in $c (non-loopback)"
        # shellcheck disable=SC2016
        cl_pxsh "$c" 'ss -tan 2>&1 | grep -v "127.0.0.1\|::1" | head -50'
    done

    for c in "$p-head" "$p-node-1" "$p-node-2"; do
        cl_hdr "spindle files in $c"
        cl_pxsh "$c" 'ls -la --time-style=+%T /tmp/commpath /tmp/spindle* 2>&1 | head -30; echo; ls -lt Spindle-build/testsuite 2>&1 | head -12'
    done

    cl_hdr "slurm daemon logs"
    # /var/log/slurm is not readable by the test user; the image gives it passwordless sudo.
    cl_pxsh "$p-head" 'sudo tail -n 25 /var/log/slurm/slurmctld.log'
    cl_pxsh "$p-node-1" 'sudo tail -n 25 /var/log/slurm/slurmd.log'

    cl_hdr "sshd log on node-1 (only the rshlaunch images run sshd)"
    cl_pxsh "$p-node-1" 'if [ -f /tmp/sshd.log ]; then sudo tail -n 30 /tmp/sshd.log; else echo "(no /tmp/sshd.log: not an rshlaunch image)"; fi'

    cl_hdr "where is it blocked?  (gdb backtraces, best effort)"
    cl_backtrace "$p-head" spindle
    cl_backtrace "$p-head" srun
    cl_backtrace "$p-node-1" spindle_be
    cl_backtrace "$p-node-1" test_driver
}

# cl_snapshot_state PREFIX DIR   -- small redacted copies of state that needs no exec
cl_snapshot_state() {
    local p=$1 sd=$2 c f id ad rt="${XDG_RUNTIME_DIR:-}"
    mkdir -p "$sd/logs" "$sd/crun" "$sd/aardvark-dns"
    podman info 2>&1 | cl_redact > "$sd/podman-info.txt"
    podman network inspect "$p-net" 2>&1 | cl_redact > "$sd/network.json"
    timeout 30 podman events --stream=false --since 2h 2>&1 | tail -n 300 | cl_redact > "$sd/events.txt"
    # crun's per-container state: what exec, stop and rm need
    if [[ -n "$rt" && -d "$rt/crun" ]]; then
        for f in "$rt"/crun/*/status; do
            [[ -f "$f" ]] || continue
            id=$(basename "$(dirname "$f")")
            mkdir -p "$sd/crun/$id"
            cl_redact < "$f" > "$sd/crun/$id/status"
        done
    fi
    ad=$(cl_aardvark_dir)
    if [[ -d "$ad" ]]; then
        for f in "$ad"/*; do
            # shellcheck disable=SC2094  # source and destination are different directories
            [[ -f "$f" ]] && cl_redact < "$f" > "$sd/aardvark-dns/$(basename "$f")"
        done
    fi
    for c in $(cl_names "$p"); do
        podman container exists "$c" 2>/dev/null || continue
        timeout 30 podman logs --tail 300 "$c" 2>&1 | cl_redact > "$sd/logs/$c.log"
    done
}

# cl_copy_spindle_logs PREFIX DIR   -- the newest Spindle debug logs from each container (needs exec)
cl_copy_spindle_logs() {
    local p=$1 dir=$2 c f i
    local members=("$p-head")
    for i in $(seq 1 "$CLUSTER_WORKERS"); do members+=("$p-node-$i"); done
    for c in "${members[@]}"; do
        # shellcheck disable=SC2016
        while IFS= read -r f; do
            [[ "$f" == /* ]] || continue
            mkdir -p "$dir/$c"
            cl_pxsh "$c" "tail -n 2000 '$f'" | cl_redact > "$dir/$c/$(basename "$f")"
        done < <(cl_pxsh "$c" 'find "$HOME" /tmp -maxdepth 4 -name "*spindle_output*" -size +0 -printf "%T@ %p\n" 2>/dev/null | sort -rn | head -6 | cut -d" " -f2-')
    done
}

# cluster_snapshot PREFIX [DIR] [REASON]
# Writes DIR/snapshot.txt, DIR/state/ and DIR/spindle-logs/, then prints where.  DIR defaults to a
# new directory inside the current run's log directory.  Safe to call on a healthy cluster.
cluster_snapshot() {
    local p=${1:-$CL_PREFIX} dir=${2:-} reason=${3:-manual} exec_ok=1
    [[ -n "$p" ]] || { echo "cluster_snapshot: a cluster prefix is required" >&2; return 1; }
    [[ -n "$dir" ]] || dir="$(cl_logdir)/snapshot-$p-$(date +%H%M%S)"
    mkdir -p "$dir" || return 1

    if ! CL_EXEC_ERR=$(timeout 30 podman exec "$p-head" true 2>&1); then
        exec_ok=0
        CL_EXEC_ERR=$(printf '%s\n' "$CL_EXEC_ERR" | head -3)
    fi

    cl_snapshot_report "$p" "$reason" "$exec_ok" 2>&1 | cl_redact > "$dir/snapshot.txt"
    cl_snapshot_state "$p" "$dir/state"
    if [[ $exec_ok == 1 ]]; then
        cl_copy_spindle_logs "$p" "$dir/spindle-logs"
    fi
    echo "snapshot written to: $dir  (snapshot.txt: $(wc -l < "$dir/snapshot.txt") lines)"
}

#------------------------------------------------------------------------------
# Running the testsuite, with a hang detector
#------------------------------------------------------------------------------

# Runs the testsuite on the head node, streaming its output.  Returns its exit status, or 124
# when no new test has started for CLUSTER_TEST_TIMEOUT seconds: then a snapshot is taken (while
# the hung test is still there) and the run is aborted.
#
# Progress means a new "Running:" line from runTests.  Other output does not count: a stuck srun
# keeps printing "step creation still disabled, retrying" forever.
#
# SPINDLE_DEBUG=N in the caller's environment is passed to the testsuite; Spindle then writes
# spindle_output.* files in Spindle-build/testsuite on each node.
cluster_run_tests() {
    local p=$1 w=$CLUSTER_WORKERS limit=$CLUSTER_TEST_TIMEOUT
    local env_args=() work pid rc=0 age current
    [[ -n "${SPINDLE_DEBUG:-}" ]] && env_args=(-e "SPINDLE_DEBUG=$SPINDLE_DEBUG")

    work=$(mktemp -d "${XDG_RUNTIME_DIR:-${TMPDIR:-/var/tmp}}/cluster-tests.XXXXXX") || return 1
    touch "$work/progress"
    echo "(before the first test)" > "$work/current"

    (
        set -o pipefail
        podman exec ${env_args[@]+"${env_args[@]}"} "$p-head" \
            bash -c "cd Spindle-build/testsuite && salloc -n$w -N$w ./runTests $w" 2>&1 |
        while IFS= read -r line; do
            printf '%s\n' "$line"
            case $line in
                Running:*) touch "$work/progress"; printf '%s\n' "$line" > "$work/current" ;;
            esac
        done
    ) 2>"$work/stderr" &
    pid=$!

    while kill -0 "$pid" 2>/dev/null; do
        sleep 1
        (( limit > 0 )) || continue
        # Compare two files' timestamps, so a clock difference with the file server cannot matter.
        touch "$work/now"
        age=$(( $(stat -c %Y "$work/now") - $(stat -c %Y "$work/progress") ))
        if (( age > limit )); then
            current=$(cat "$work/current")
            cl_err "no new test started for ${age}s (CLUSTER_TEST_TIMEOUT=$limit); last started: $current"
            cluster_snapshot "$p" "" "no new test for ${age}s; last started: $current"
            pkill -P "$pid" 2>/dev/null
            pkill -u "$(id -u)" -f "podman exec .*$p-head" 2>/dev/null
            rc=124
            break
        fi
    done

    if (( rc == 0 )); then
        wait "$pid"
        rc=$?
    else
        wait "$pid" 2>/dev/null
    fi
    # The shell's own report of the killed pipeline ("Terminated | while ...") is noise after a hang
    # abort; any other stderr from the test subshell is shown.
    if (( rc != 124 )) && [[ -s "$work/stderr" ]]; then cat "$work/stderr" >&2; fi
    [[ -d "$work" ]] && rm -rf -- "$work"
    return $rc
}
