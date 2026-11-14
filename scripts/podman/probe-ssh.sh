#!/bin/bash
#
# probe-ssh.sh - Can sshd accept a login inside a rootless-podman container?
#
# Starts sshd in a throwaway container and logs in to it with a key, twice:
#   1. with podman's default user/group mapping
#   2. with the extra mapping from LLNL's "containers gotchas" page (uid/gid 65534 mapped)
# and reports which one works.
#
# Why: sshd drops to an unprivileged user (Ubuntu: `sshd`, primary group `nogroup` = gid 65534) for
# every connection.  A rootless container's default mapping has no gid 65534, so setgroups() fails
# and the connection is reset.  Slurm's rsh launch mode (Spindle starting its daemons over ssh) hits this.
#
# Usage: scripts/podman/probe-ssh.sh [image]        (default: spindle-slurm-base)
# Needs: the image (with openssh-server and the slurmuser account).  Takes about 30 seconds.

set -u

IMAGE="${1:-spindle-slurm-base}"
MAPS=(--uidmap 0:0:2000 --uidmap 65534:2000:2 --gidmap 0:0:2000 --gidmap 65534:2000:2)

command -v podman >/dev/null || { echo "podman not found"; exit 2; }
if ! podman image exists "$IMAGE" 2>/dev/null; then
    echo "Image '$IMAGE' not found.  Build it with: ./spindle-podman build slurm-base"
    exit 2
fi

# Runs inside the container as root.  Single-quoted on purpose: nothing here expands on the host.
# shellcheck disable=SC2016
INNER='
echo "openssh:   $(ssh -V 2>&1)"
echo "sshd user: $(getent passwd sshd)"
echo "uid_map:   $(tr -s " " < /proc/self/uid_map | tr "\n" ";")"
echo "gid_map:   $(tr -s " " < /proc/self/gid_map | tr "\n" ";")"
mkdir -p /run/sshd
ssh-keygen -A >/dev/null 2>&1
/usr/sbin/sshd -E /tmp/sshd.log -o LogLevel=DEBUG1 -o PasswordAuthentication=no || { echo "sshd failed to start"; cat /tmp/sshd.log; exit 90; }
sleep 1
su slurmuser -c "mkdir -p ~/.ssh && ssh-keygen -q -t ed25519 -N \"\" -f ~/.ssh/id_ed25519 && cp ~/.ssh/id_ed25519.pub ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys && ssh -o StrictHostKeyChecking=no -o BatchMode=yes -o ConnectTimeout=5 localhost \"echo LOGIN-OK as \$(id -un)\""
rc=$?
echo "ssh exit status: $rc"
echo "--- sshd log, last 12 lines"
tail -n 12 /tmp/sshd.log
exit $rc
'

run_case() {   # run_case LABEL [podman options...]
    local label=$1 out rc
    shift
    echo "=========================================="
    echo "$label"
    echo "=========================================="
    local t0=$SECONDS
    out=$(timeout 300 podman run --rm --entrypoint bash "$@" "$IMAGE" -c "$INNER" 2>&1)
    rc=$?
    echo "  (container run took $((SECONDS - t0)) s; a custom id mapping can make the first start slow, because podman must remap the image's file ownership)"
    printf '%s\n' "$out" | sed 's/^/  /'
    echo ""
    if printf '%s' "$out" | grep -q 'LOGIN-OK'; then CASE_OK=1; else CASE_OK=0; fi
    return $rc
}

run_case "1. default mapping" ; DEFAULT_OK=$CASE_OK
run_case "2. with --uidmap/--gidmap for 65534 (${MAPS[*]})" "${MAPS[@]}"; MAPPED_OK=$CASE_OK

echo "=========================================="
echo "Result"
echo "=========================================="
echo "  default mapping:  $([ "$DEFAULT_OK" = 1 ] && echo 'login works' || echo 'login FAILS')"
echo "  with 65534 maps:  $([ "$MAPPED_OK" = 1 ] && echo 'login works' || echo 'login FAILS')"
echo ""
if [ "$DEFAULT_OK" = 1 ]; then
    echo "ssh works with the default mapping: the old rshlaunch failure was something else."
elif [ "$MAPPED_OK" = 1 ]; then
    echo "ssh fails by default and works with the extra mapping: run the sshd containers with"
    echo "  ${MAPS[*]}"
    echo "(the slurm-rshlaunch target sets exactly this in T_RUN_ARGS, scripts/podman/targets.sh)."
else
    echo "ssh fails either way.  Read the sshd log excerpts above; the usual suspects are the"
    echo "mapping range (your /etc/subuid allows 2048 ids) and sshd's own sandbox restrictions."
fi

[ "$MAPPED_OK" = 1 ] || [ "$DEFAULT_OK" = 1 ]
