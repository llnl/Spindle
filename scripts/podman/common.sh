#!/bin/bash
#
# Common functions for running Spindle containers with podman on LC systems
#
# LC systems require special handling for:
# 1. SSL certificates (volume mounts)
# 2. apt setgroups errors (handled via Dockerfile ARG)

set -e

# LC-specific SSL certificate mounts
# Build: Mount LLNL cert into ca-certificates directory
LC_CERT_BUILD_MOUNT="-v /etc/pki/ca-trust/source/anchors/PAN-cspca.llnl.gov.crt.pem:/usr/local/share/ca-certificates/cspca.crt:ro"

# Run: Mount system CA bundle
LC_CERT_RUN_MOUNT="-v /etc/pki/tls/certs/ca-bundle.trust.crt:/etc/ssl/certs/ca-certificates.crt:ro"

# Helper function to build images with LC-specific settings
podman_build() {
    local image_name=$1
    local dockerfile=$2
    local context=${3:-.}
    local extra_args="$4"  # Optional extra build args

    echo "========================================"
    echo "Building: $image_name"
    echo "Dockerfile: $dockerfile"
    echo "Context: $context"
    echo "========================================"

    podman build \
        --build-arg PODMAN_BUILD=true \
        $LC_CERT_BUILD_MOUNT \
        $extra_args \
        -t "$image_name" \
        -f "$dockerfile" \
        "$context"

    local rc=$?
    if [ $rc -eq 0 ]; then
        echo "✓ Build successful: $image_name"
    else
        echo "✗ Build failed with exit code $rc"
        return $rc
    fi
}

# Helper function to run containers with LC-specific settings
podman_run() {
    local image_name=$1
    shift

    echo "========================================"
    echo "Running: $image_name"
    echo "========================================"

    podman run \
        --rm \
        $LC_CERT_RUN_MOUNT \
        "$image_name" \
        "$@"

    local rc=$?
    if [ $rc -eq 0 ]; then
        echo "✓ Container exited successfully"
    else
        echo "✗ Container exited with code $rc"
        return $rc
    fi
}

# Helper function to clean up images
podman_cleanup() {
    local image_name=$1
    echo "Cleaning up image: $image_name"
    podman rmi "$image_name" 2>/dev/null || true
}
