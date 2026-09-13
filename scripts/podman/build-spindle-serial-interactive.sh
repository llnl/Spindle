#!/bin/bash
#
# Build Spindle serial-interactive container for podman
#
# This builds a serial container that stays running for interactive debugging.
# Run this from outside the sandbox where podman is available.

set -e

# Get the directory containing this script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Source common functions
source "$SCRIPT_DIR/common.sh"

# Configuration
IMAGE_NAME="spindle-serial-interactive-ubuntu"
DOCKERFILE="$REPO_ROOT/containers/spindle-serial-interactive-ubuntu/Dockerfile.podman"

echo "=========================================="
echo "Building Spindle Serial Interactive Container"
echo "=========================================="
echo ""
echo "This builds a serial container for interactive debugging."
echo "Image: $IMAGE_NAME"
echo "Dockerfile: $DOCKERFILE"
echo ""

# Build the image
podman_build "$IMAGE_NAME" "$DOCKERFILE" "$REPO_ROOT"

echo ""
echo "=========================================="
echo "Build complete!"
echo "=========================================="
echo ""
echo "Image: $IMAGE_NAME"
echo ""
echo "Next steps:"
echo "  1. Start container: ./scripts/podman/run-spindle-serial-interactive.sh"
echo "  2. Connect to it: podman exec -it spindlenode-interactive bash"
echo "  3. Run tests manually: cd Spindle-build/testsuite && SPINDLE_DEBUG=3 ./runTests"
echo ""
