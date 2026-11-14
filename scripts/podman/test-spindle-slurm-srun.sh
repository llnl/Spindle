#!/bin/bash
#
# Run the Spindle testsuite under Slurm (srun launcher) in a rootless-podman cluster.
#
# Usage: test-spindle-slurm-srun.sh [run-id]
#   run-id             suffix for container and network names (default 1), so runs can coexist
#   CLUSTER_KEEP=1     leave the cluster running afterwards, for inspection
#   CLUSTER_WORKERS=N  number of slurmd workers (default 4; the image is built with 4)
#
# Needs the spindle-slurm-srun and mariadb:12 images on this node.  Run it outside the sandbox.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/podman/cluster.sh
source "$SCRIPT_DIR/cluster.sh"

RUN_ID="${1:-1}"
PREFIX="slurm-srun-${RUN_ID}"
IMAGE="spindle-slurm-srun"

echo "=========================================="
echo "Spindle Slurm srun tests (run $RUN_ID)"
echo "=========================================="

if ! podman image exists "$IMAGE" 2>/dev/null; then
    echo "Error: image $IMAGE not found on this node.  Build it with: ./spindle-podman build slurm-srun"
    exit 1
fi
if ! podman image exists "$CLUSTER_MARIADB_IMAGE" 2>/dev/null \
        && ! podman image exists "docker.io/library/$CLUSTER_MARIADB_IMAGE" 2>/dev/null; then
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
    echo "✓ Testsuite passed (run $RUN_ID)"
elif [[ $rc -eq 124 ]]; then
    echo "✗ Testsuite hung (run $RUN_ID): a snapshot was taken; see the snapshot-* directory in .podman-logs/"
    echo "  Re-run with --keep to leave the cluster up, then: ./spindle-podman diagnose hang slurm-srun-$RUN_ID"
else
    echo "✗ Testsuite failed with status $rc (run $RUN_ID)"
    echo "  Re-run with --keep (CLUSTER_KEEP=1) to leave the cluster up for inspection."
fi
exit $rc
