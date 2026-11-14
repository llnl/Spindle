#!/bin/bash
#
# Target registry for spindle-podman.  Sourced, not executed.
#
# To add a target: append its name to TARGETS and set the T_* fields below.
#
#   T_IMAGE       image name, without "localhost/"             required, unless T_PULL
#   T_DOCKERFILE  Dockerfile path, relative to the repo root   required, unless T_PULL
#   T_PULL        image reference to `podman pull` instead of  default: none (built)
#                 building (set it INSTEAD of T_IMAGE/T_DOCKERFILE)
#   T_CONTEXT     build context, relative to the repo root     default: repo root
#   T_BUILD_ARGS  extra `podman build` options (word-split)    default: none
#   T_RUN_ARGS    extra `podman run` options for the Slurm     default: none
#                 containers of a cluster target (word-split)
#   T_CLUSTER_NAME  stem of the cluster's container names:     default: the target name
#                 <stem>-<run id>-node-N, -head, -db, ...
#   T_NODE_NAME_MAX  longest node name the target can cope     default: no limit
#                 with; the test script refuses to start past it
#   T_PREREQ      targets whose images must exist first        default: none
#                 (space-separated; fetched in order if missing)
#   T_PRESTEP     shell command run in repo root before build  default: none
#   T_BASELINE    expected build time in seconds; warn if      default: no warning
#                 exceeded
#   T_TEST        test script, relative to the repo root       default: not testable
#   T_DESC        one-line description for `help`              required

# shellcheck disable=SC2034  # all variables here are consumed by the scripts that source this file
declare -A T_IMAGE T_DOCKERFILE T_PULL T_CONTEXT T_BUILD_ARGS T_RUN_ARGS T_CLUSTER_NAME T_NODE_NAME_MAX T_PREREQ T_PRESTEP T_BASELINE T_TEST T_DESC

TARGETS=(serial slurm-base mariadb slurm-srun slurm-rshlaunch slurm-plugin)

T_IMAGE[serial]=spindle-serial-ubuntu
T_DOCKERFILE[serial]=containers/spindle-serial-ubuntu/Dockerfile.podman
T_BASELINE[serial]=450      # 13Nov2026 rzadams: observed 300s
T_TEST[serial]=scripts/podman/test-spindle-serial.sh
T_DESC[serial]="Serial resource manager (MPI via mpirun)"

T_IMAGE[slurm-base]=spindle-slurm-base
T_DOCKERFILE[slurm-base]=containers/spindle-slurm-ubuntu/base/Dockerfile.podman
T_CONTEXT[slurm-base]=containers/spindle-slurm-ubuntu/base
T_BASELINE[slurm-base]=700  # 13Nov2026 rzadams: observed 580s; earlier runs 506-721s
T_DESC[slurm-base]="Slurm + MPICH base image (slow build, rarely needed)"

# Pinned, not "latest": a moving tag could change between allocations.  The tests' database image
# (cluster.sh reads this reference).
T_PULL[mariadb]=docker.io/library/mariadb:12
T_DESC[mariadb]="MariaDB database image for the Slurm clusters"

T_IMAGE[slurm-srun]=spindle-slurm-srun
T_DOCKERFILE[slurm-srun]=containers/spindle-slurm-ubuntu/testing-srun/Dockerfile.podman
T_BUILD_ARGS[slurm-srun]="--build-arg replicas=4"
T_PREREQ[slurm-srun]="mariadb slurm-base"
T_BASELINE[slurm-srun]=300  # 13Nov2026 rzadams: observed 217-220s
T_TEST[slurm-srun]=scripts/podman/test-spindle-slurm.sh
T_DESC[slurm-srun]="Slurm cluster, srun launcher (needs the slurm-base and mariadb images)"

# Slurm with Spindle starting its daemons over ssh (--with-rsh-launch).  sshd drops privileges to
# uid/gid 65534 for every login, which rootless podman does not map by default, so logins were reset
# ("Connection reset by ... port 22").  The maps below are from LLNL's container "gotchas" page; the
# ranges must fit inside /etc/subuid (2048 ids here).  `spindle-podman diagnose ssh` tests this.
T_IMAGE[slurm-rshlaunch]=spindle-slurm-rshlaunch
T_DOCKERFILE[slurm-rshlaunch]=containers/spindle-slurm-ubuntu/testing/Dockerfile.podman
T_BUILD_ARGS[slurm-rshlaunch]="--build-arg replicas=4"
T_RUN_ARGS[slurm-rshlaunch]="--uidmap 0:0:2000 --uidmap 65534:2000:2 --gidmap 0:0:2000 --gidmap 65534:2000:2"
T_PREREQ[slurm-rshlaunch]="mariadb slurm-base"
T_BASELINE[slurm-rshlaunch]=400  # 13Nov2026 rzadams: observed 289s
T_TEST[slurm-rshlaunch]=scripts/podman/test-spindle-slurm.sh
T_DESC[slurm-rshlaunch]="Slurm cluster, Spindle daemons launched over ssh (needs slurm-base and mariadb)"

# Slurm with Spindle loaded as a SPANK plugin in slurmd (--enable-slurm-plugin); its daemons start from
# the job prolog.  No ssh, so no uid maps.  The CI job also runs session and crash tests; only the
# main testsuite is run here so far.
T_IMAGE[slurm-plugin]=spindle-slurm-plugin
T_DOCKERFILE[slurm-plugin]=containers/spindle-slurm-ubuntu/testing-plugin/Dockerfile.podman
T_BUILD_ARGS[slurm-plugin]="--build-arg replicas=4"
T_PREREQ[slurm-plugin]="mariadb slurm-base"
# Spindle's plugin asks `sinfo -O NodeAddr` for each node's address, and sinfo cuts that column to 20
# characters unless told otherwise (src/slurm_plugin/plugin_utils.c, getHostAddrSinfo).  A longer node
# name comes back truncated ("slurm-plugin-1-node-"), the front end cannot resolve it, and the first
# test hangs.  Found 13Nov2026.  CI's names ("slurm-node-1") are short.  So: short names here.
# Fixing the Spindle code would make this unnecessary (then delete these two lines).
T_CLUSTER_NAME[slurm-plugin]=slurm-plug
T_NODE_NAME_MAX[slurm-plugin]=20
T_BASELINE[slurm-plugin]=400  # 13Nov2026 rzadams: observed 256-284s
T_TEST[slurm-plugin]=scripts/podman/test-spindle-slurm.sh
T_DESC[slurm-plugin]="Slurm cluster, Spindle as a SPANK plugin (needs slurm-base and mariadb)"

# Name prefix of the clusters created by test scripts (<prefix>-<id>-head, ...); `cleanup`
# removes their networks.  Containers are matched by the "spindle|slurm" name filter.
KNOWN_NETWORK_PREFIXES=(slurm-srun slurm-rshlaunch slurm-plug slurm-plugin)
