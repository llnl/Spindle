#!/bin/bash
#
# Run Spindle serial-interactive container for manual debugging
#
# This starts the container and keeps it running for interactive access.
# Run this from outside the sandbox where podman is available.

set -e

# Get the directory containing this script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Source common functions
source "$SCRIPT_DIR/common.sh"

# Configuration
IMAGE_NAME="spindle-serial-interactive-ubuntu"
CONTAINER_NAME="spindlenode-interactive"

echo "=========================================="
echo "Spindle Serial Interactive Container"
echo "=========================================="
echo ""

# Check if container already exists
if podman ps -a --format "{{.Names}}" 2>/dev/null | grep -q "^${CONTAINER_NAME}\$"; then
    echo "Container '$CONTAINER_NAME' already exists."
    echo ""

    # Check if it's running
    if podman ps --format "{{.Names}}" 2>/dev/null | grep -q "^${CONTAINER_NAME}\$"; then
        echo "Container is running. Connect with:"
        echo "  podman exec -it $CONTAINER_NAME bash"
    else
        echo "Container exists but is stopped. Removing..."
        podman rm -f "$CONTAINER_NAME" 2>/dev/null
        echo "Starting fresh container..."
    fi
    echo ""
fi

# Only start if not already running
if ! podman ps --format "{{.Names}}" 2>/dev/null | grep -q "^${CONTAINER_NAME}\$"; then
    echo "Starting interactive container..."
    echo ""

    podman run \
        --name "$CONTAINER_NAME" \
        --hostname "$CONTAINER_NAME" \
        --cap-add SYS_NICE \
        -d \
        -t \
        "$IMAGE_NAME"

    echo "✓ Container started"
    sleep 3

    echo ""
    echo "=========================================="
    echo "Container ready for debugging!"
    echo "=========================================="
    echo ""
fi

echo "Connect to the container:"
echo "  podman exec -it $CONTAINER_NAME bash"
echo ""
echo "Inside the container, you can:"
echo "  1. Check munge: munge -n | unmunge"
echo "  2. Enable core dumps: ulimit -c unlimited"
echo "  3. Run tests: cd Spindle-build/testsuite && SPINDLE_DEBUG=3 ./runTests"
echo "  4. Check cores: ls -lh /tmp/core.* Spindle-build/testsuite/core.*"
echo "  5. Debug with gdb: gdb ./test_driver /tmp/core.test_driver.PID"
echo ""
echo "When done, stop the container:"
echo "  podman rm -f $CONTAINER_NAME"
echo ""
