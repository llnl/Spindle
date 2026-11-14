#!/bin/bash
#
# Run the Spindle testsuite under Slurm in a rootless-podman cluster.  One script for every Slurm
# flavor: the target (slurm-srun, slurm-rshlaunch, ...) comes from SPINDLE_PODMAN_TARGET, which
# `spindle-podman test` sets; the image and per-target container options come from targets.sh.
#
# Usage: test-spindle-slurm.sh [run-id]
#   run-id             suffix for container and network names (default 1), so runs can coexist
#   CLUSTER_KEEP=1     leave the cluster running afterwards, for inspection
#   CLUSTER_WORKERS=N  number of slurmd workers (default 4; the image is built with 4)
#
# Needs the target's image and the mariadb image on this node.  Run it outside the sandbox.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/podman/cluster.sh disable=SC1091
source "$SCRIPT_DIR/cluster.sh"

TARGET="${SPINDLE_PODMAN_TARGET:-slurm-srun}"
if [[ -z "${T_IMAGE[$TARGET]:-}" ]]; then
    echo "Error: '$TARGET' is not a target with an image (see scripts/podman/targets.sh)"
    exit 1
fi
RUN_ID="${1:-1}"
PREFIX="${T_CLUSTER_NAME[$TARGET]:-$TARGET}-${RUN_ID}"
IMAGE="${T_IMAGE[$TARGET]}"
# Per-target container options (e.g. uid maps).  Set CLUSTER_RUN_ARGS yourself, even to the empty
# string, to try something else:   CLUSTER_RUN_ARGS= ./spindle-podman test slurm-rshlaunch
export CLUSTER_RUN_ARGS="${CLUSTER_RUN_ARGS-${T_RUN_ARGS[$TARGET]:-}}"

echo "=========================================="
echo "Spindle $TARGET tests (run $RUN_ID)"
echo "=========================================="
echo "cluster name: $PREFIX   extra podman run options for the Slurm containers: ${CLUSTER_RUN_ARGS:-<none>}"

# Some targets cannot cope with long node names (see T_NODE_NAME_MAX in targets.sh); stop before 10
# minutes of hanging tests, not after.
if [[ -n "${T_NODE_NAME_MAX[$TARGET]:-}" ]]; then
    longest="$PREFIX-node-$CLUSTER_WORKERS"
    if (( ${#longest} > T_NODE_NAME_MAX[$TARGET] )); then
        echo "Error: node name '$longest' is ${#longest} characters; $TARGET needs at most ${T_NODE_NAME_MAX[$TARGET]}."
        echo "       Use a shorter run id (--id), or fewer workers."
        exit 1
    fi
fi

if ! podman image exists "$IMAGE" 2>/dev/null; then
    echo "Error: image $IMAGE not found on this node.  Build it with: ./spindle-podman build $TARGET"
    exit 1
fi
if ! podman image exists "$CLUSTER_MARIADB_IMAGE" 2>/dev/null; then
    echo "Error: image $CLUSTER_MARIADB_IMAGE not found on this node (image storage is node-local)."
    echo "Get it with: ./spindle-podman build mariadb     (or ./spindle-podman load)"
    exit 1
fi

trap 'cluster_down "$PREFIX"' EXIT
trap 'exit 130' INT TERM

if ! cluster_up "$PREFIX" "$IMAGE"; then
    echo ""
    echo "✗ Cluster did not come up; no tests were run."
    exit 1
fi

echo ""
echo "=========================================="
echo "Running the Spindle testsuite"
echo "=========================================="
cluster_run_tests "$PREFIX"
rc=$?

echo ""
if [[ $rc -eq 0 ]]; then
    echo "✓ Testsuite passed ($PREFIX)"
elif [[ $rc -eq 124 ]]; then
    echo "✗ Testsuite hung ($PREFIX): a snapshot was taken; see the snapshot-* directory in .podman-logs/"
    echo "  Re-run with --keep to leave the cluster up, then: ./spindle-podman diagnose hang $PREFIX"
else
    echo "✗ Testsuite failed with status $rc ($PREFIX)"
    echo "  Re-run with --keep (CLUSTER_KEEP=1) to leave the cluster up for inspection."
fi
exit $rc
