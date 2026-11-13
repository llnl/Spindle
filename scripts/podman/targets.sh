#!/bin/bash
#
# Target registry for spindle-podman.  Sourced, not executed.
#
# To add a target: append its name to TARGETS and set the T_* fields below.
#
#   T_IMAGE       image name, without "localhost/"             required
#   T_DOCKERFILE  Dockerfile path, relative to the repo root   required
#   T_CONTEXT     build context, relative to the repo root     default: repo root
#   T_BUILD_ARGS  extra `podman build` options (word-split)    default: none
#   T_PREREQ      target whose image must exist first          default: none
#   T_PRESTEP     shell command run in repo root before build  default: none
#   T_BASELINE    expected build time in seconds; warn if      default: no warning
#                 exceeded
#   T_TEST        test script, relative to the repo root       default: not testable
#   T_DESC        one-line description for `help`              required

# shellcheck disable=SC2034  # all variables here are consumed by the scripts that source this file
declare -A T_IMAGE T_DOCKERFILE T_CONTEXT T_BUILD_ARGS T_PREREQ T_PRESTEP T_BASELINE T_TEST T_DESC

TARGETS=(serial slurm-base)

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

# Networks created by test scripts; removed by `cleanup`.
KNOWN_NETWORKS=(slurm-rshlaunch-test-net slurm-srun-test-net)
