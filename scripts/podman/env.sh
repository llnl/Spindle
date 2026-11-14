#!/bin/bash
#
# env.sh - pin the directory podman keeps its runtime state in.  Sourced; safe to source twice.
#
# Why: podman and crun keep per-container state under $XDG_RUNTIME_DIR.  When /run/user/<uid>
# does not exist (it appears only while a login session does), podman falls back to a TMPDIR-based
# path.  If another login then creates /run/user/<uid>, later podman commands look there instead,
# and exec/stop/rm on containers started earlier fail with
#     crun: error opening file `/run/user/<uid>/crun/<id>/status': No such file or directory
# (diagnosed 13Nov2026 from probe-dns.sh and the hung srun run; not independently verified).
# A fixed, node-local directory makes every shell agree.
#
# Override with SPINDLE_PODMAN_RUNTIME_DIR.  Give another shell the same value with:
#     eval "$(./spindle-podman env)"

SPINDLE_PODMAN_RUNTIME_DIR="${SPINDLE_PODMAN_RUNTIME_DIR:-/var/tmp/${USER:-$(id -un)}/xdg-runtime}"

if mkdir -p "$SPINDLE_PODMAN_RUNTIME_DIR" 2>/dev/null && chmod 700 "$SPINDLE_PODMAN_RUNTIME_DIR" 2>/dev/null; then
    export XDG_RUNTIME_DIR="$SPINDLE_PODMAN_RUNTIME_DIR"
else
    echo "warning: cannot create $SPINDLE_PODMAN_RUNTIME_DIR; XDG_RUNTIME_DIR stays ${XDG_RUNTIME_DIR:-<unset>}" >&2
fi
