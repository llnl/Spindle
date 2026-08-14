#!/bin/bash
#
# Run N parallel Spindle Slurm srun tests in podman
#
# Usage: test-spindle-slurm-srun.sh <num-instances>
#
# Creates N independent Slurm clusters and runs tests in parallel.
# Each instance outputs to both stdout and out.<N> with timestamps.
#
# Run this from outside the sandbox where podman is available.

set -e

NUM_INSTANCES="${1}"

if [ -z "$NUM_INSTANCES" ]; then
    echo "Usage: $0 <num-instances>"
    echo ""
    echo "Example: $0 10"
    echo "  Creates 10 independent Slurm clusters and runs tests in parallel"
    exit 1
fi

if ! [[ "$NUM_INSTANCES" =~ ^[0-9]+$ ]] || [ "$NUM_INSTANCES" -lt 1 ]; then
    echo "Error: num-instances must be a positive integer"
    exit 1
fi

# Get the directory containing this script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Source common functions
source "$SCRIPT_DIR/common.sh"

# Configuration
IMAGE_NAME="spindle-slurm-srun"
WORKERS=4

echo "=========================================="
echo "Spindle Slurm Srun Parallel Tests"
echo "=========================================="
echo ""
echo "Instances: $NUM_INSTANCES"
echo "Each cluster: MariaDB + slurmdbd + slurmctld + 4 workers"
echo "Output: stdout + out.<N> files (timestamped)"
echo ""

# Function to run a single test instance
run_instance() {
    local INSTANCE_ID=$1
    local NAME_PREFIX="slurm-srun-${INSTANCE_ID}"

    # Instance-specific hostnames for shared network
    local MARIADB_HOST="${NAME_PREFIX}-mariadb"
    local DB_HOST="${NAME_PREFIX}-db"
    local HEAD_NODE="${NAME_PREFIX}-head"
    local NODE_PREFIX="${NAME_PREFIX}-node"

    # All output from this function goes through ts and tee
    {
        echo "[Instance $INSTANCE_ID] Starting test"
        echo ""

        echo "[Instance $INSTANCE_ID] Setting up Slurm cluster..."
        echo ""

        # Generate random password for this cluster
        MARIADB_PASSWORD=$(openssl rand -base64 16)
        echo "[Instance $INSTANCE_ID] Generated MariaDB password"
        echo ""

        # Start MariaDB
        echo "[Instance $INSTANCE_ID] Starting MariaDB..."
        podman run \
            --name "${NAME_PREFIX}-mariadb" \
            --hostname "$MARIADB_HOST" \
            --network "$SHARED_NETWORK" \
            -e MYSQL_RANDOM_ROOT_PASSWORD=yes \
            -e MYSQL_DATABASE=slurm_acct_db \
            -e MYSQL_USER=slurm \
            -e MYSQL_PASSWORD="$MARIADB_PASSWORD" \
            -d \
            mariadb:12 >/dev/null
        echo "[Instance $INSTANCE_ID] MariaDB started"
        echo "[Instance $INSTANCE_ID] Waiting for MariaDB to initialize (15s)..."
        sleep 15
        echo ""

        # Start worker nodes FIRST so they're available for DNS resolution
        # when slurmctld starts and tries to resolve node addresses
        echo "[Instance $INSTANCE_ID] Starting worker nodes..."
        for i in $(seq 1 $WORKERS); do
            echo "[Instance $INSTANCE_ID] Starting ${NODE_PREFIX}-$i..."
            podman run \
                --name "${NAME_PREFIX}-node-$i" \
                --hostname "${NODE_PREFIX}-$i" \
                --network "$SHARED_NETWORK" \
                -e SLURM_ROLE=worker \
                -e SLURM_HEAD_NODE="$HEAD_NODE" \
                -e SLURM_DB_HOST="$DB_HOST" \
                -e SLURM_NODE_PREFIX="$NODE_PREFIX" \
                -e workers="$WORKERS" \
                -d \
                "$IMAGE_NAME" >/dev/null
            echo "[Instance $INSTANCE_ID] ${NODE_PREFIX}-$i started"
        done
        echo "[Instance $INSTANCE_ID] Waiting for workers to initialize (5s)..."
        sleep 5
        echo ""

        # Start slurmdbd
        echo "[Instance $INSTANCE_ID] Starting slurmdbd..."
        podman run \
            --name "${NAME_PREFIX}-db" \
            --hostname "$DB_HOST" \
            --network "$SHARED_NETWORK" \
            -e SLURM_ROLE=db \
            -e SLURM_HEAD_NODE="$HEAD_NODE" \
            -e SLURM_DB_HOST="$DB_HOST" \
            -e SLURM_MARIADB_HOST="$MARIADB_HOST" \
            -e SLURM_NODE_PREFIX="$NODE_PREFIX" \
            -e workers="$WORKERS" \
            -e MARIADB_PASSWORD="$MARIADB_PASSWORD" \
            -d \
            "$IMAGE_NAME" >/dev/null
        echo "[Instance $INSTANCE_ID] slurmdbd started"
        sleep 10
        echo ""

        # Start slurmctld LAST so all other containers are reachable via DNS
        echo "[Instance $INSTANCE_ID] Starting slurmctld..."
        podman run \
            --name "${NAME_PREFIX}-head" \
            --hostname "$HEAD_NODE" \
            --network "$SHARED_NETWORK" \
            -e SLURM_ROLE=ctl \
            -e SLURM_HEAD_NODE="$HEAD_NODE" \
            -e SLURM_DB_HOST="$DB_HOST" \
            -e SLURM_NODE_PREFIX="$NODE_PREFIX" \
            -e workers="$WORKERS" \
            -t \
            -d \
            "$IMAGE_NAME" >/dev/null
        echo "[Instance $INSTANCE_ID] slurmctld started"
        sleep 10
        echo ""

        echo "[Instance $INSTANCE_ID] Waiting for Slurm cluster to initialize (60s)..."
        sleep 60
        echo ""

        # Verify cluster
        echo "[Instance $INSTANCE_ID] Verifying cluster..."
        if podman exec "${NAME_PREFIX}-head" sinfo >/dev/null 2>&1; then
            echo "[Instance $INSTANCE_ID] Cluster ready"
        else
            echo "[Instance $INSTANCE_ID] WARNING: sinfo failed"
            echo ""
            echo "[Instance $INSTANCE_ID] ========== DIAGNOSTICS =========="

            # Check container status
            echo "[Instance $INSTANCE_ID] Container status:"
            for container in ${NAME_PREFIX}-mariadb ${NAME_PREFIX}-db ${NAME_PREFIX}-head; do
                if podman ps --filter "name=$container" --format "{{.Names}}" | grep -q "$container"; then
                    echo "[Instance $INSTANCE_ID]   ✓ $container is running"
                else
                    echo "[Instance $INSTANCE_ID]   ✗ $container has exited!"
                fi
            done
            echo ""

            # Show MariaDB logs
            echo "[Instance $INSTANCE_ID] MariaDB logs (last 20 lines):"
            podman logs "${NAME_PREFIX}-mariadb" 2>&1 | tail -20 | sed "s/^/[Instance $INSTANCE_ID]   /"
            echo ""

            # Show slurmdbd logs
            echo "[Instance $INSTANCE_ID] slurmdbd logs (last 30 lines):"
            podman logs "${NAME_PREFIX}-db" 2>&1 | tail -30 | sed "s/^/[Instance $INSTANCE_ID]   /"
            echo ""

            # Show slurmctld logs
            echo "[Instance $INSTANCE_ID] slurmctld logs (last 30 lines):"
            podman logs "${NAME_PREFIX}-head" 2>&1 | tail -30 | sed "s/^/[Instance $INSTANCE_ID]   /"
            echo ""

            # Test MariaDB connectivity
            echo "[Instance $INSTANCE_ID] Testing MariaDB connectivity:"
            if podman exec "${NAME_PREFIX}-mariadb" mysqladmin ping 2>&1 | grep -q "mysqld is alive"; then
                echo "[Instance $INSTANCE_ID]   ✓ MariaDB is responding"
            else
                echo "[Instance $INSTANCE_ID]   ✗ MariaDB not responding"
            fi
            echo ""

            # Check if slurmdbd can resolve MariaDB hostname
            echo "[Instance $INSTANCE_ID] DNS check from slurmdbd:"
            podman exec "${NAME_PREFIX}-db" getent hosts slurm-mariadb 2>&1 | sed "s/^/[Instance $INSTANCE_ID]   /" || echo "[Instance $INSTANCE_ID]   ✗ Cannot resolve slurm-mariadb"
            echo ""

            echo "[Instance $INSTANCE_ID] ========== END DIAGNOSTICS =========="
            echo ""
            echo "[Instance $INSTANCE_ID] Continuing with tests anyway..."
        fi
        echo ""

        # Run tests
        echo "[Instance $INSTANCE_ID] Running Spindle testsuite..."
        echo "[Instance $INSTANCE_ID] Press any key to run tests..."
        read -n 1 -s -r < /dev/tty
        echo ""
        if podman exec "${NAME_PREFIX}-head" bash -c "cd Spindle-build/testsuite && export SPINDLE_DEBUG=3 && salloc -n${WORKERS} -N${WORKERS} ./runTests ${WORKERS}"; then
            echo ""
            echo "[Instance $INSTANCE_ID] =========================================="
            echo "[Instance $INSTANCE_ID] ALL TESTS PASSED"
            echo "[Instance $INSTANCE_ID] =========================================="
            RESULT=0
        else
            echo ""
            echo "[Instance $INSTANCE_ID] =========================================="
            echo "[Instance $INSTANCE_ID] SOME TESTS FAILED"
            echo "[Instance $INSTANCE_ID] =========================================="
            RESULT=1
        fi
        echo ""
        echo "[Instance $INSTANCE_ID] Test complete"
        echo "[Instance $INSTANCE_ID] Containers are running. You can now:"
        echo "[Instance $INSTANCE_ID]   podman exec -it ${NAME_PREFIX}-head bash"
        echo "[Instance $INSTANCE_ID]   podman exec -it ${NAME_PREFIX}-node-1 bash"
        echo "[Instance $INSTANCE_ID] Press any key to cleanup and exit..."
        read -n 1 -s -r < /dev/tty
        echo ""
        echo "[Instance $INSTANCE_ID] Cleanup will happen in serial phase"
        echo ""

        exit $RESULT
    } 2>&1 | ts | tee "out.${INSTANCE_ID}"
}

# Shared network for all instances (avoids 30s timeout per instance)
SHARED_NETWORK="slurm-srun-shared"

# Serial phase: Verify prerequisites and cleanup
echo "=========================================="
echo "Serial Phase: Prerequisites & Cleanup"
echo "=========================================="
echo ""

echo "Checking for required images..."
if ! podman images | grep -q "spindle-slurm-srun"; then
    echo "ERROR: spindle-slurm-srun image not found"
    echo "Build it first with: ./build-spindle-slurm-srun.sh"
    exit 1
fi

if ! podman images | grep -q "mariadb.*12"; then
    echo "ERROR: mariadb:12 image not found"
    echo "Pull it first with: podman pull mariadb:12"
    echo "Or load from tarball with: ./load-images.sh <tarball>"
    exit 1
fi
echo "✓ All required images present"
echo ""

echo "Cleaning up any existing test containers and network..."
for i in $(seq 1 $NUM_INSTANCES); do
    NAME_PREFIX="slurm-srun-${i}"

    # Remove containers for this instance
    for container in ${NAME_PREFIX}-mariadb ${NAME_PREFIX}-db ${NAME_PREFIX}-head ${NAME_PREFIX}-node-{1..4}; do
        podman rm -f "$container" 2>/dev/null || true
    done
done

# Remove shared network
podman network rm -f "$SHARED_NETWORK" 2>/dev/null || true
echo "✓ Cleanup complete"
echo ""

echo "Creating shared network: $SHARED_NETWORK"
podman network create "$SHARED_NETWORK" >/dev/null
echo "✓ Shared network created (this may take ~30s due to systemd session bus timeout)"
echo ""

# Parallel phase: Launch all instances
echo "=========================================="
echo "Parallel Phase: Launching $NUM_INSTANCES instances"
echo "=========================================="
echo ""

PIDS=()
for i in $(seq 1 $NUM_INSTANCES); do
    echo "Launching instance $i..."
    run_instance $i &
    PIDS+=($!)
done

echo ""
echo "All instances launched. Waiting for completion..."
echo "(Output to stdout and out.<N> files)"
echo ""

# Wait for all instances and collect results
FAILED=0
for i in $(seq 1 $NUM_INSTANCES); do
    if ! wait ${PIDS[$((i-1))]}; then
        FAILED=$((FAILED + 1))
    fi
done

echo ""
echo "=========================================="
echo "Serial Phase: Cleanup"
echo "=========================================="
echo ""

echo "Cleaning up $NUM_INSTANCES test clusters..."
for i in $(seq 1 $NUM_INSTANCES); do
    NAME_PREFIX="slurm-srun-${i}"

    echo "Cleaning up instance $i..."

    # Stop containers
    for container in ${NAME_PREFIX}-mariadb ${NAME_PREFIX}-db ${NAME_PREFIX}-head ${NAME_PREFIX}-node-{1..4}; do
        podman stop "$container" 2>/dev/null || true
    done

    # Remove containers
    for container in ${NAME_PREFIX}-mariadb ${NAME_PREFIX}-db ${NAME_PREFIX}-head ${NAME_PREFIX}-node-{1..4}; do
        podman rm -f "$container" 2>/dev/null || true
    done
done
echo "✓ Cleanup complete"
echo ""

# Remove shared network
echo "Removing shared network..."
podman network rm -f "$SHARED_NETWORK" 2>/dev/null || true
echo "✓ Network removed"
echo ""

# Serial phase: Summary
echo "=========================================="
echo "Serial Phase: Summary"
echo "=========================================="
echo ""
echo "Total instances: $NUM_INSTANCES"
echo "Passed: $((NUM_INSTANCES - FAILED))"
echo "Failed: $FAILED"
echo ""

if [ $FAILED -eq 0 ]; then
    echo "✓ All instances passed!"
    exit 0
else
    echo "✗ Some instances failed"
    echo ""
    echo "Check individual logs:"
    for i in $(seq 1 $NUM_INSTANCES); do
        if grep -q "SOME TESTS FAILED" "out.$i" 2>/dev/null; then
            echo "  out.$i - FAILED"
        elif grep -q "ALL TESTS PASSED" "out.$i" 2>/dev/null; then
            echo "  out.$i - PASSED"
        else
            echo "  out.$i - UNKNOWN"
        fi
    done
    exit 1
fi
