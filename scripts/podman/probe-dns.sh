#!/bin/bash
#
# probe-dns.sh - Why does (or doesn't) name-based container DNS work under this rootless podman?
#
# Creates a throwaway network and two containers (A runs a tiny TCP listener, B is the client),
# then tests each layer separately so a failure can be localized:
#
#   1. IP reachability   B -> A by IP (TCP connect; ping is informational only, because
#                        containers.conf drops NET_RAW and ping needs it)
#   2. The resolver      B's resolver (the network gateway): TCP/53 open?  dig/nslookup answers?
#   3. Host-side state   Is aardvark-dns running?  What does it have registered for this network?
#   4. Name lookup       getent hosts by container name, --hostname, and network alias
#   5. /etc/hosts        Inject A's IP into B's /etc/hosts, then resolve and connect by name
#
# Usage: scripts/podman/probe-dns.sh [image]        (default: spindle-slurm-base)
#   PROBE_TIMEOUT=N   seconds to wait for each lookup / connect (default 5)
#
# Everything created is named spindle-dnsprobe-<pid> and removed on exit.
# Image must contain bash, timeout, python3 (for the listener); dig/ip/ping are used if present.

set -u

IMAGE="${1:-spindle-slurm-base}"
TIMEOUT="${PROBE_TIMEOUT:-5}"
PORT=8080
ID="$$"
NET="spindle-dnsprobe-${ID}"
A="spindle-dnsprobe-${ID}-a"
B="spindle-dnsprobe-${ID}-b"
A_HOSTNAME="probehost${ID}"             # deliberately different from A's container name
A_ALIAS="probealias${ID}"

now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

MANUAL_AV_PID=""
AV_STARTED_BY_US=""
AV_CFG=""
C="spindle-dnsprobe-${ID}-c"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOGDIR="$REPO_ROOT/.podman-logs"
LOGFILE="$LOGDIR/dnsprobe-${ID}.log"
SYSTEMD_DIR="${PROBE_SYSTEMD_DIR:-/run/systemd/system}"

# aardvark-dns daemonizes, so killing the launcher is not enough: match the server by its arguments.
stop_manual_aardvark() {
    if [[ -n "$MANUAL_AV_PID" ]]; then kill "$MANUAL_AV_PID" 2>/dev/null; fi
    MANUAL_AV_PID=""
    if [[ -n "$AV_STARTED_BY_US" && -n "$AV_CFG" ]]; then
        pkill -u "$(id -u)" -f "aardvark-dns --config $AV_CFG -p 53" 2>/dev/null
    fi
    AV_STARTED_BY_US=""
}

cleanup() {
    stop_manual_aardvark
    podman rm -f -t 0 "$A" "$B" "$C" >/dev/null 2>&1
    podman network rm -f "$NET" >/dev/null 2>&1
}
trap cleanup EXIT INT TERM

FAIL=0
RESULTS=()

record() {   # record <label> <ok|fail|info> <detail>
    [[ "$2" == fail ]] && FAIL=$((FAIL + 1))
    RESULTS+=("$(printf '%-5s %-38s %s' "$(echo "$2" | tr '[:lower:]' '[:upper:]')" "$1" "$3")")
}

result_is() {   # result_is <ok|fail> <label-fragment>
    printf '%s\n' "${RESULTS[@]}" | grep -q "^$(echo "$1" | tr '[:lower:]' '[:upper:]') .*$2"
}

section() { echo ""; echo "--- $*"; }

# container_ip <container>   IPv4 address, read from inside the container.
# (`podman inspect` reported no IP under podman 5.3.1 + netavark + slirp4netns.)
container_ip() {
    local ip
    ip=$(podman exec "$1" sh -c "ip -4 -o addr show scope global 2>/dev/null | awk '{print \$4}' | cut -d/ -f1 | head -1" 2>/dev/null)
    [[ -z "$ip" ]] && ip=$(podman inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$1" 2>/dev/null)
    echo "$ip"
}

# tcp_ok <from-container> <host> <port>   one TCP connect attempt, 3 s limit
tcp_ok() {
    podman exec "$1" timeout 3 bash -c "exec 3<>/dev/tcp/$2/$3" >/dev/null 2>&1
}

# tcp_wait <from-container> <host> <port>   retry until TIMEOUT; sets ELAPSED_MS
tcp_wait() {
    local start; start=$(now_ms)
    while :; do
        tcp_ok "$1" "$2" "$3" && { ELAPSED_MS=$(( $(now_ms) - start )); return 0; }
        ELAPSED_MS=$(( $(now_ms) - start ))
        (( ELAPSED_MS > TIMEOUT * 1000 )) && return 1
        sleep 0.5
    done
}

# resolve_wait <from-container> <name>   sets RESOLVED_IP, ELAPSED_MS
resolve_wait() {
    local start out; start=$(now_ms); RESOLVED_IP=""
    while :; do
        if out=$(podman exec "$1" getent hosts "$2" 2>/dev/null) && [[ -n "$out" ]]; then
            RESOLVED_IP="${out%% *}"; ELAPSED_MS=$(( $(now_ms) - start )); return 0
        fi
        ELAPSED_MS=$(( $(now_ms) - start ))
        (( ELAPSED_MS > TIMEOUT * 1000 )) && return 1
        sleep 0.5
    done
}

echo "=========================================="
echo "Container DNS probe"
echo "=========================================="
echo "Image:   $IMAGE"
echo "Network: $NET"
echo "Timeout: ${TIMEOUT}s per check"

command -v podman >/dev/null || { echo "podman not found"; exit 2; }
if ! podman image exists "$IMAGE" 2>/dev/null; then
    echo "Image '$IMAGE' not found.  Build it (e.g. ./spindle-podman build slurm-base) or pass another image."
    exit 2
fi

section "environment"
echo "podman:             $(podman --version)"
if command -v jq >/dev/null; then
    podman info --format json 2>/dev/null | jq -r '
        "network backend:    \(.host.networkBackend // "unknown")",
        "netavark:           \(.host.networkBackendInfo.path // "n/a") \(.host.networkBackendInfo.version // "")",
        "aardvark-dns:       \(.host.networkBackendInfo.dns.path // "n/a") \(.host.networkBackendInfo.dns.version // "")",
        "rootless net cmd:   \(.host.rootlessNetworkCmd // "unknown")",
        "runroot:            \(.store.runRoot // "unknown")"'
fi
AARDVARK_BIN=""
command -v jq >/dev/null && AARDVARK_BIN=$(podman info --format json 2>/dev/null | jq -r '.host.networkBackendInfo.dns.path // ""')
for tool in pasta slirp4netns; do
    printf '%-19s %s\n' "$tool:" "$(command -v "$tool" || echo 'not found on PATH')"
done
echo "CONTAINERS_CONF:    ${CONTAINERS_CONF:-<unset>}"
echo "XDG_RUNTIME_DIR:    ${XDG_RUNTIME_DIR:-<unset>}"

section "step 1: create network"
t0=$(now_ms)
podman network create "$NET" >/dev/null 2>&1 || { echo "network create FAILED"; exit 1; }
NET_MS=$(( $(now_ms) - t0 ))
GATEWAY=""; DNS_ENABLED=unknown
if command -v jq >/dev/null; then
    insp=$(podman network inspect "$NET" 2>/dev/null)
    DNS_ENABLED=$(echo "$insp" | jq -r '.[0].dns_enabled // "unknown"')
    GATEWAY=$(echo "$insp" | jq -r '.[0].subnets[0].gateway // ""')
fi
echo "created in ${NET_MS} ms; dns_enabled=${DNS_ENABLED}; gateway=${GATEWAY:-unknown}"
record "network create" ok "${NET_MS} ms"
if [[ "$DNS_ENABLED" == false ]]; then
    record "network has dns_enabled" fail "dns_enabled=false"
else
    record "network has dns_enabled" ok "$DNS_ENABLED"
fi

section "step 2: start containers"
t0=$(now_ms)
STARTED=1
if ! A_START=$(podman --log-level=debug run -d --name "$A" --hostname "$A_HOSTNAME" --network "$NET" \
        --network-alias "$A_ALIAS" --entrypoint bash "$IMAGE" -c \
        "if command -v python3 >/dev/null; then exec python3 -m http.server $PORT --bind 0.0.0.0; else exec sleep 300; fi" 2>&1); then
    echo "starting container A FAILED:"; echo "$A_START" | tail -5 | sed 's/^/    /'
    record "start container A" fail ""; STARTED=0
fi
if ! B_START=$(podman run -d --name "$B" --network "$NET" --entrypoint sleep "$IMAGE" 300 2>&1); then
    echo "starting container B FAILED:"; echo "$B_START" | tail -5 | sed 's/^/    /'
    record "start container B" fail ""; STARTED=0
fi
START_MS=$(( $(now_ms) - t0 ))
echo "A: name=$A hostname=$A_HOSTNAME alias=$A_ALIAS  (TCP listener on :$PORT)"
echo "B: name=$B"
echo "started in ${START_MS} ms"

if [[ $STARTED -eq 1 ]]; then
    record "start containers" ok "${START_MS} ms"
    A_IP=$(container_ip "$A")
    B_IP=$(container_ip "$B")
    echo "A's IP: ${A_IP:-unknown}    B's IP: ${B_IP:-unknown}   (read from inside each container)"
    INSPECT_IP=$(podman inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$A" 2>/dev/null)
    record "podman inspect reports A's IP" info "${INSPECT_IP:-empty}"
    if [[ -z "$INSPECT_IP" ]]; then
        echo "podman inspect's view of A's networks (why is the IP empty?):"
        podman inspect -f '{{json .NetworkSettings.Networks}}' "$A" 2>&1 | cut -c1-600 | sed 's/^/    /'
    fi
    echo ""
    echo "podman/netavark messages about aardvark/DNS/systemd while starting A (firewall chatter filtered out;"
    echo "full debug log: $LOGFILE):"
    mkdir -p "$LOGDIR"
    printf '%s\n' "$A_START" > "$LOGFILE"
    A_MSGS=$(grep -iE 'aardvark|dns|systemd|scope|spawn|bus|slirp|warn|error|fail' "$LOGFILE" | grep -v 'netavark::firewall' | cut -c1-260 | head -40)
    if [[ -n "$A_MSGS" ]]; then printf '%s\n' "$A_MSGS" | sed 's/^/    /'; else echo "    (none)"; fi

    section "step 3: layer 1 - IP reachability (no names involved)"
    echo "B's interfaces and routes:"
    podman exec "$B" sh -c 'ip -br addr 2>/dev/null; ip route 2>/dev/null' | sed 's/^/    /'
    if [[ -z "$A_IP" ]]; then
        record "A has an IP address" fail "podman inspect gave none"
    else
        if podman exec "$B" ping -c 1 -W 2 "$A_IP" >/dev/null 2>&1; then
            record "ping A by IP (informational)" info "reply"
        else
            record "ping A by IP (informational)" info "no reply (NET_RAW is dropped in containers.conf, so this proves little)"
        fi
        if tcp_wait "$A" "127.0.0.1" "$PORT"; then
            record "control: A's listener up (loopback)" ok "${ELAPSED_MS} ms"
        else
            record "control: A's listener up (loopback)" fail "listener not running; B->A results are unreliable"
        fi
        if tcp_wait "$B" "$A_IP" "$PORT"; then
            echo "  TCP connect B -> $A_IP:$PORT ok in ${ELAPSED_MS} ms"
            record "TCP B -> A by IP" ok "${ELAPSED_MS} ms"
        else
            echo "  TCP connect B -> $A_IP:$PORT FAILED after ${ELAPSED_MS} ms"
            record "TCP B -> A by IP" fail "no connection in ${TIMEOUT}s"
        fi
        if [[ -z "$GATEWAY" ]]; then
            record "ping gateway (informational)" info "gateway unknown"
        elif podman exec "$B" ping -c 1 -W 2 "$GATEWAY" >/dev/null 2>&1; then
            record "ping gateway (informational)" info "reply"
        else
            record "ping gateway (informational)" info "no reply"
        fi
    fi

    section "step 4: layer 2 - the resolver B is told to use"
    echo "B's /etc/resolv.conf:"
    podman exec "$B" cat /etc/resolv.conf 2>/dev/null | sed 's/^/    /'
    RESOLVER=$(podman exec "$B" sh -c "awk '/^nameserver/{print \$2; exit}' /etc/resolv.conf" 2>/dev/null | head -1)
    echo "resolver: ${RESOLVER:-unknown}"
    if [[ -n "$RESOLVER" ]]; then
        if tcp_ok "$B" "$RESOLVER" 53; then
            record "TCP connect to resolver :53" ok "$RESOLVER"
        else
            record "TCP connect to resolver :53" fail "$RESOLVER refused or silent"
        fi
        if podman exec "$B" sh -c 'command -v dig' >/dev/null 2>&1; then
            echo "dig @$RESOLVER $A  (udp):"
            podman exec "$B" dig +time=2 +tries=1 "@$RESOLVER" "$A" +noall +comments +answer 2>&1 | sed 's/^/    /' | head -12
            echo "dig @$RESOLVER $A  (tcp):"
            podman exec "$B" dig +tcp +time=2 +tries=1 "@$RESOLVER" "$A" +noall +comments +answer 2>&1 | sed 's/^/    /' | head -12
        elif podman exec "$B" sh -c 'command -v nslookup' >/dev/null 2>&1; then
            podman exec "$B" nslookup -timeout=2 "$A" "$RESOLVER" 2>&1 | sed 's/^/    /' | head -12
        else
            echo "  (neither dig nor nslookup in image)"
        fi
    fi

    section "step 5: layer 3 - host-side aardvark-dns state"
    echo "aardvark-dns processes owned by you:"
    pgrep -a -u "$(id -u)" aardvark-dns 2>/dev/null | sed 's/^/    /' || true
    if pgrep -u "$(id -u)" aardvark-dns >/dev/null 2>&1; then
        record "aardvark-dns process running" ok ""
    else
        record "aardvark-dns process running" fail "none found for uid $(id -u)"
    fi
    echo "inside the rootless network namespace:"
    podman unshare --rootless-netns sh -c 'ip -br addr; echo; ss -lnup 2>/dev/null; ss -lntp 2>/dev/null' 2>&1 | sed 's/^/    /' | head -30
    echo "aardvark-dns data files for this network:"
    RUNROOT=$(podman info --format '{{.Store.RunRoot}}' 2>/dev/null)
    found=0; AV_CFG=""
    declare -A seen=()
    for root in "$RUNROOT" "$(dirname "${RUNROOT:-/nonexistent}")" "${XDG_RUNTIME_DIR:-/nonexistent}"; do
        [[ -d "$root" ]] || continue
        while IFS= read -r f; do
            [[ -n "${seen[$f]:-}" ]] && continue
            seen[$f]=1; found=1; AV_CFG=$(dirname "$f")
            echo "  $f"; sed 's/^/      /' "$f" 2>/dev/null | head -10
        done < <(find "$root" -maxdepth 5 -path '*aardvark-dns*' -name "$NET" 2>/dev/null)
    done
    [[ $found -eq 0 ]] && echo "  (none found under runroot, its parent, or XDG_RUNTIME_DIR)"

    if [[ -n "$AV_CFG" ]]; then
        echo "everything in the aardvark-dns directory ($AV_CFG):"
        find "$AV_CFG" -maxdepth 1 -mindepth 1 -printf '    %M %u %TY-%Tm-%Td %TH:%TM  %s bytes  %f\n' 2>/dev/null | sort -k5
        if [[ -f "$AV_CFG/aardvark.pid" ]]; then
            AV_PIDFILE=$(tr -d '[:space:]' < "$AV_CFG/aardvark.pid" 2>/dev/null)
            echo "aardvark.pid contains: ${AV_PIDFILE:-<empty>}"
            if [[ "$AV_PIDFILE" =~ ^[0-9]+$ ]] && kill -0 "$AV_PIDFILE" 2>/dev/null; then
                ps -o pid,etime,comm,args -p "$AV_PIDFILE" 2>/dev/null | sed 's/^/    /'
                if [[ "$(ps -o comm= -p "$AV_PIDFILE" 2>/dev/null)" == aardvark-dns ]]; then
                    record "aardvark.pid names a live aardvark-dns" info "pid $AV_PIDFILE"
                else
                    record "aardvark.pid names a live NON-aardvark process" fail \
                        "pid $AV_PIDFILE is $(ps -o comm= -p "$AV_PIDFILE" 2>/dev/null): netavark may signal the wrong process"
                fi
            else
                echo "    no such process (stale pid file)"
                record "aardvark.pid is stale (process gone)" fail "pid ${AV_PIDFILE:-?}"
            fi
        else
            echo "no aardvark.pid in that directory"
            record "aardvark.pid present" info "none"
        fi
    fi

    section "step 5a: how netavark launches aardvark-dns"
    SYSD=0
    if [[ -d "$SYSTEMD_DIR" ]]; then
        SYSD=1
        echo "$SYSTEMD_DIR exists.  netavark's binary contains 'systemd-run -q --scope --user'; when systemd is"
        echo "running it appears to wrap aardvark-dns in a user scope, which needs the user's systemd manager"
        echo "(reached over its bus).  Testing that mechanism directly:"
    else
        echo "$SYSTEMD_DIR not present: netavark should exec aardvark-dns directly (no systemd-run)."
    fi
    echo "XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-<unset>}   DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS:-<unset>}"
    stat -c "    %A %U %n" "/run/user/$(id -u)" "/run/user/$(id -u)/bus" 2>&1
    if [[ $SYSD -eq 1 ]] && command -v systemd-run >/dev/null; then
        t0=$(now_ms)
        out=$(timeout 40 systemd-run -q --scope --user true 2>&1); rc=$?
        ms=$(( $(now_ms) - t0 ))
        echo "  systemd-run -q --scope --user true   -> status $rc in ${ms} ms"
        if [[ -n "$out" ]]; then printf '%s\n' "$out" | sed 's/^/      /'; fi
        if [[ $rc -eq 0 ]]; then
            record "systemd-run --user --scope works" ok "${ms} ms"
        else
            record "systemd-run --user --scope works" fail "status $rc after ${ms} ms"
        fi
        if [[ -z "${XDG_RUNTIME_DIR:-}" && -d "/run/user/$(id -u)" ]]; then
            t0=$(now_ms)
            out=$(XDG_RUNTIME_DIR="/run/user/$(id -u)" timeout 40 systemd-run -q --scope --user true 2>&1); rc=$?
            ms=$(( $(now_ms) - t0 ))
            echo "  same, with XDG_RUNTIME_DIR=/run/user/$(id -u)  -> status $rc in ${ms} ms"
            if [[ -n "$out" ]]; then printf '%s\n' "$out" | sed 's/^/      /'; fi
            if [[ $rc -eq 0 ]]; then
                record "systemd-run works with XDG_RUNTIME_DIR set" ok "${ms} ms"
            else
                record "systemd-run works with XDG_RUNTIME_DIR set" fail "status $rc after ${ms} ms"
            fi
        fi
    fi

    section "step 5b: does aardvark-dns serve when started by hand?  (it daemonizes, so poll for the process)"
    if pgrep -u "$(id -u)" aardvark-dns >/dev/null 2>&1; then
        echo "  an aardvark-dns process already exists; not starting another"
    elif [[ -z "$AARDVARK_BIN" || ! -x "$AARDVARK_BIN" ]]; then
        echo "  aardvark-dns binary not found (podman info reported: '${AARDVARK_BIN:-none}')"
    elif [[ -z "$AV_CFG" ]]; then
        echo "  no aardvark config directory found, so there is nothing to serve"
    else
        AV_STARTED_BY_US=1
        AV_LOG=$(mktemp)
        echo "  $AARDVARK_BIN --config $AV_CFG -p 53 run   (inside the rootless netns)"
        timeout 120 podman unshare --rootless-netns "$AARDVARK_BIN" --config "$AV_CFG" -p 53 run >"$AV_LOG" 2>&1 &
        MANUAL_AV_PID=$!
        up=0
        for _ in 1 2 3 4 5 6 7 8; do
            sleep 0.5
            if pgrep -u "$(id -u)" aardvark-dns >/dev/null 2>&1; then up=1; break; fi
        done
        if kill -0 "$MANUAL_AV_PID" 2>/dev/null; then
            echo "  launcher still running: the server stays in the foreground"
        else
            wait "$MANUAL_AV_PID"; lrc=$?
            echo "  launcher exited with status $lrc (expected 0 if the server daemonized)"
        fi
        if [[ -s "$AV_LOG" ]]; then echo "  launcher output:"; sed 's/^/    /' "$AV_LOG" | head -20; fi
        if [[ $up -eq 1 ]]; then
            echo "  aardvark-dns is running:"
            pgrep -a -u "$(id -u)" aardvark-dns | sed 's/^/    /'
            echo "  listeners in the netns:"
            podman unshare --rootless-netns sh -c 'ss -lnup 2>/dev/null; ss -lntp 2>/dev/null' 2>&1 | sed 's/^/    /' | head -12
            if resolve_wait "$B" "$A"; then
                echo "  B resolved $A -> $RESOLVED_IP with the hand-started server"
                record "aardvark works when started by hand" ok "$RESOLVED_IP (${ELAPSED_MS} ms)"
            else
                echo "  B still cannot resolve $A even with it running"
                record "aardvark works when started by hand" fail "running, but no answer"
            fi
            echo "  starting container C now that the server is up (tests whether netavark reloads a running server):"
            if podman run -d --name "$C" --network "$NET" --entrypoint sleep "$IMAGE" 300 >/dev/null 2>&1; then
                if resolve_wait "$B" "$C"; then
                    record "late container resolvable (hand-started aardvark)" ok "$RESOLVED_IP (${ELAPSED_MS} ms)"
                else
                    record "late container resolvable (hand-started aardvark)" fail "C started, but B cannot resolve it"
                fi
            else
                record "late container resolvable (hand-started aardvark)" fail "could not start C"
            fi
        else
            record "aardvark works when started by hand" fail "no aardvark-dns process appeared within 4 s"
        fi
        stop_manual_aardvark
        rm -f "$AV_LOG"
    fi

    section "step 6: layer 4 - name lookup (getent) from B"
    for pair in "container name:$A" "--hostname:$A_HOSTNAME" "network alias:$A_ALIAS"; do
        label="${pair%%:*}"; name="${pair#*:}"
        if resolve_wait "$B" "$name"; then
            echo "  resolved ($label) $name -> $RESOLVED_IP in ${ELAPSED_MS} ms"
            record "resolve by $label" ok "$RESOLVED_IP (${ELAPSED_MS} ms)"
        else
            echo "  FAILED   ($label) $name after ${ELAPSED_MS} ms"
            record "resolve by $label" fail "no answer in ${TIMEOUT}s"
        fi
    done

    section "step 7: layer 5 - /etc/hosts fallback"
    if [[ -n "$A_IP" ]]; then
        if printf '%s %s %s %s\n' "$A_IP" "$A" "$A_HOSTNAME" "$A_ALIAS" \
                | podman exec --user root -i "$B" tee -a /etc/hosts >/dev/null 2>&1; then
            if resolve_wait "$B" "$A_HOSTNAME" && [[ "$RESOLVED_IP" == "$A_IP" ]]; then
                record "resolve via injected /etc/hosts" ok "$RESOLVED_IP"
            else
                record "resolve via injected /etc/hosts" fail "entry written but lookup failed"
            fi
            if tcp_wait "$B" "$A_HOSTNAME" "$PORT"; then
                record "TCP B -> A by name via /etc/hosts" ok "${ELAPSED_MS} ms"
            else
                record "TCP B -> A by name via /etc/hosts" fail "no connection"
            fi
        else
            record "write to B's /etc/hosts" fail "tee failed"
        fi
    fi
fi

echo ""
echo "=========================================="
echo "Results"
echo "=========================================="
printf '%s\n' "${RESULTS[@]}"
echo ""

echo "Interpretation:"
if [[ $STARTED -eq 0 ]]; then
    echo "  Setup failed before anything could be tested; see messages above."
elif result_is fail "A has an IP address"; then
    echo "  Could not read an IP address for container A, so the reachability tests did not run."
    echo "  Check the interface listing in step 3 and the inspect output in step 2."
elif result_is fail "control: A's listener"; then
    echo "  A's own TCP listener never came up (python3 missing in the image?), so the B -> A"
    echo "  results above prove nothing.  Pass an image with python3, or fix A's command."
elif result_is fail "TCP B -> A by IP"; then
    echo "  Containers cannot reach each other even by IP, so this is NOT just a DNS problem."
    echo "  /etc/hosts injection will not help.  Look at the rootless-netns output in step 5"
    echo "  (is the bridge up? any listeners?) and at B's routes in step 3."
elif result_is ok "TCP B -> A by IP"; then
    names_ok=1
    for l in "container name" "--hostname" "network alias"; do result_is ok "resolve by $l" || names_ok=0; done
    if [[ $names_ok -eq 1 ]]; then
        echo "  Routing and name lookup both work.  No /etc/hosts workaround is needed."
    else
        echo "  Routing works; name resolution does not.  Containers can talk by IP."
        if result_is ok "resolve via injected /etc/hosts"; then
            echo "  The /etc/hosts fallback works end to end, so the cluster helper can use it."
        fi
        if result_is fail "aardvark-dns process running"; then
            echo "  No aardvark-dns process is running for you: it is not being started, or dies."
            if result_is fail "aardvark.pid is stale"; then
                echo "  A stale aardvark.pid was found in the aardvark-dns directory (see step 5).  Netavark may"
                echo "  mistake it for a running server.  Try removing that directory's pid/lock files with no"
                echo "  containers running, then rerun the probe."
            fi
            if result_is fail "aardvark.pid names a live NON-aardvark"; then
                echo "  aardvark.pid names a live process that is not aardvark-dns, so netavark believes a server"
                echo "  exists and signals the wrong process instead of starting one.  Remove the pid file."
            fi
            if result_is ok "aardvark works when started by hand"; then
                echo "  Started by hand, aardvark-dns serves correctly: the server is fine and podman/netavark"
                echo "  is failing to launch it."
                if result_is ok "late container resolvable"; then
                    echo "  A running server also picks up containers started afterwards, so the cluster helper can"
                    echo "  pre-start it (podman unshare --rootless-netns aardvark-dns ...) and use real DNS."
                elif result_is fail "late container resolvable"; then
                    echo "  But a container started after the server was up was not registered; see step 5b."
                fi
                if result_is fail "systemd-run --user --scope works"; then
                    echo "  systemd-run --user --scope fails here (step 5a): the likely reason netavark cannot"
                    echo "  launch aardvark-dns."
                fi
            elif result_is fail "aardvark works when started by hand"; then
                echo "  Starting it by hand does not produce a working server; the step 5b output is the thing to chase."
            else
                echo "  Step 5b did not run (see its message above)."
            fi
        elif result_is fail "TCP connect to resolver"; then
            echo "  aardvark-dns is running but nothing accepts TCP on the resolver address."
            echo "  Compare the listeners in step 5 with the resolver address in step 4."
        else
            echo "  aardvark-dns is running and reachable but not answering for these names;"
            echo "  compare its data file in step 5 with the container names."
        fi
    fi
fi

if [[ $(printf '%s\n' "${RESULTS[@]}" | grep -c '^FAIL') -gt 0 ]] && ! result_is ok "TCP B -> A by IP" \
        && ! result_is fail "TCP B -> A by IP" && ! result_is fail "control" && ! result_is fail "A has an IP"; then
    echo "  The checks above did not reach a conclusion; read the sections above."
fi

[[ $FAIL -eq 0 ]]
