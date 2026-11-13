#!/bin/bash
#
# Build Spindle Slurm base image for podman
#
# This builds the base image with Slurm and MPICH.
# Run this from outside the sandbox where podman is available.

set -e

# Get the directory containing this script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Source common functions
source "$SCRIPT_DIR/common.sh"

# Configuration
IMAGE_NAME="spindle-slurm-base"
DOCKERFILE="$REPO_ROOT/containers/spindle-slurm-ubuntu/base/Dockerfile.podman"
CONTEXT="$REPO_ROOT/containers/spindle-slurm-ubuntu/base"

echo "=========================================="
echo "Building Spindle Slurm Base Image"
echo "=========================================="
echo ""
echo "This builds the base image with Slurm and MPICH."
echo "This will take 5-10 minutes (building Slurm from source)."
echo "Image: $IMAGE_NAME"
echo "Dockerfile: $DOCKERFILE"
echo ""

# Build the image
podman_build "$IMAGE_NAME" "$DOCKERFILE" "$CONTEXT"

echo ""
echo "=========================================="
echo "Build complete!"
echo "=========================================="
echo ""
echo "Image: $IMAGE_NAME"
echo ""
echo "Next step: Build the testing image"
echo "  ./scripts/podman/build-spindle-slurm-srun.sh"
echo ""
