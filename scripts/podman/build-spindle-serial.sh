#!/bin/bash
#
# Build Spindle serial container for podman
#
# This builds the actual Spindle serial container (not a hello-world demo).
# Run this from outside the sandbox where podman is available.

set -e

# Get the directory containing this script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Source common functions
source "$SCRIPT_DIR/common.sh"

# Configuration
IMAGE_NAME="spindle-serial-ubuntu"
DOCKERFILE="$REPO_ROOT/containers/spindle-serial-ubuntu/Dockerfile.podman"

echo "=========================================="
echo "Building Spindle Serial Container"
echo "=========================================="
echo ""
echo "This builds the actual Spindle serial test container."
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
echo "  1. Run regular tests: ./scripts/podman/test-spindle-serial.sh"
echo "  2. Run crash tests: ./scripts/podman/test-spindle-serial-crash.sh"
echo "  3. Or manually: podman run --rm -it $IMAGE_NAME"
echo ""
