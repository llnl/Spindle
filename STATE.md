The next lesson in learning how to be a software developer is
leaving enough of a paper trail so that I be pulled off of a
branch for a month and be able to pick back up where I left off.

# Goal for this branch
Create rootless-podman containers to allow CI/CD of Spindle
running under slurm, slurm-plugin, flux, and serial resource
managers on LC machines.  The model to use is the containers
used by the Github workflows. Those containers can run with
root privs.  That's not a possibility locally, which makes
this hard.

# Rules for LLMs
1. You are running in a sandbox that prevents most internet
and filesystem access.

2. If a tool isn't available, please ask.  If it's not on
the system (or not accessible) we can probably built it from
source.

3. Don't run any `git` commands that alter state (no commit,
push, rebase, etc.).

4. The directory you're working under is
    `/p/vast1/rountree/rzadams/sandbox/workspace-Spindle`
I have a softlink in my home directory, so you may see:
    `/g/g24/rountree/v/rzadams/sandbox/workspace-Spindle`
or
    `~/v/rzadams/sandbox/workspace-Spindle`
They are a equivalent for me.  Some may work better for you
than others.

5. The sandbox has a quirk that you're allowed to write to
certain forbidden directories (and the writes will succeed),
but the writes are only visible to you.  This is a particular
problem for the `/tmp` directory.  If you need a scratch
directory, create one under the sandbox.

6. We're running on a cluster with login and compute nodes.
Compute nodes can only be reserved for a few hours at a time.
Modest amounts of work can be done on login nodes, but assume
any process running on a login node that is taking up significant
CPU time will be terminated after 60 seconds.

7. Compute nodes and login nodes both have the `/p/vast1` filesystem
mounted, but `/tmp` and `$TMPDIR` are local.  Anything cached in
RAM (say, podman builds) will disappear when the compute node
allocation ends.

8. If a compute node is available, I'll have you executing on it,
so feel free to use as many cores and as much memory as you like.
If you're running on a login node, I'll let you know.  If you want
to check for yourself, the login nodes' hostnames are `rzadams1001`
and `rzadams1017`.

9. Don't write to this file.

# State
    ✓ `spindle-podman status`
    ✓ `spindle-podman help`
    ✓ `spindle-podman save`
    ✓ `spindle-podman load`
    ✓ `diagnose ssh`

    ✓ `spindle-podman build serial`
    ✓ `spindle-podman build mariadb`
    ✓ `spindle-podman build slurm-base`
    ✓ `spindle-podman build slurm-srun`
    ✓ `build slurm-rshlaunch` (289s) 

    ✓ `spindle-podman test serial`
    ✓ `spindle-podman test slurm-srun`
    ✓ `test slurm-rshlaunch` (200s)


# Manifest (last updated 13Nov2026)
        containers/spindle-slurm-ubuntu/
        containers/spindle-slurm-ubuntu/
        containers/spindle-slurm-ubuntu/
        scripts/podman/probe-ssh.sh
        scripts/podman/

All new unless marked (modified).  Files with a .podman suffix are podman variants of devel's
files; devel's originals are untouched, so the GitHub workflows are unaffected.

spindle-podman                  The one entry point (bash): status build test save load env
                                diagnose logs cleanup defenestrate help.  `help` is the documentation.
STATE.md                        This file.
.dockerignore                   Keeps the image tarball, logs and git metadata out of the build
                                context (the context is the repo root).
.gitignore (modified)           Ignores .podman-state.json, .podman-metrics.log, .podman-logs/.

scripts/podman/
  targets.sh                    Target registry: image, Dockerfile, prerequisites, expected build
                                time, test script.  Add a target here, not in spindle-podman.
  env.sh                        Pins XDG_RUNTIME_DIR (/var/tmp/$USER/xdg-runtime).  Sourced.
  common.sh                     LC certificate mounts for builds.  Its podman_build/podman_run
                                helpers are mostly unused now.
  cluster.sh                    Library: Slurm cluster up/down (network, MariaDB, hand-started
                                aardvark-dns, workers, slurmdbd, slurmctld, readiness polling),
                                the testsuite runner with hang detection, and snapshots.
  test-spindle-serial.sh        Serial test: one container, munge check, testsuite.
  test-spindle-slurm-srun.sh    srun test: cluster_up, testsuite, cluster_down (always).
  probe-dns.sh                  `diagnose dns`: tests container name resolution layer by layer.
  test-spindle-slurm.sh

containers/spindle-serial-ubuntu/
  Dockerfile.podman             Serial image.  PODMAN_BUILD=true works around apt's setgroups error.
  scripts/entrypoint.sh.podman  Starts munged in the background and sleeps (devel's runs it in the
                                foreground, which would end the container).
containers/spindle-slurm-ubuntu/
  base/Dockerfile.podman        Slurm + MPICH base image.
  base/scripts/build_mpich.sh.podman
                                tar --no-same-owner, which rootless podman needs.
  testing-srun/Dockerfile.podman
                                srun image: copies templates, builds Spindle, and FAILS the build
                                if Spindle was configured without --external-launcher.
  testing-srun/conf/slurm.conf.template.podman
  testing-srun/conf/slurmdbd.conf.template.podman
                                Hostname placeholders, filled in at container start.
                                TaskPlugin=task/none (task/affinity fails in rootless podman).
  testing-srun/scripts/setup_slurm.sh.podman
                                Installs the templates plus a stub slurm.conf, so `srun --version`
                                works when Spindle is configured (see 13Nov2026, srun passes).
  testing-srun/scripts/entrypoint.sh.podman
                                Renders the configs from the templates, then starts the daemon
                                named by SLURM_ROLE (db, ctl, worker).  Never prints the password.
  testing-srun/scripts/check_launch_mode.sh.podman
  testing/Dockerfile.podman
  testing/scripts/entrypoint.sh.podman
Generated, not committed:
  .podman-state.json  .podman-metrics.log  .podman-logs/    State, timings, per-run logs and snapshots.
  <workspace>/tmp/podman-images/spindle-images.tar          Saved images (3.9 GB), outside the repo.


#  Next
rshlaunch, then flux, then slurm plugin as new targets.  rshlaunch's old failure was
"Connection reset ... port 22"; the LLNL doc on uid maps (--uidmap 0:0:2000
--uidmap 65534:2000:2) is an untested lead for sshd.  `--repeat` on srun to find flakiness.

# Diary

## Preamble

Starting the fourth attempt...
In the previous `podman-port` branch I did a rebase that ended
up scrambling the hashes for subsequent commits.  I also failed to
keep track of files associated with earlier attempts, newer files,
debugging work, etc.  Today's task is to start at the top with the
`spindle-podman` script and just pull over what it needs.  The
previous branch has been renamed `podman-port-old`.  The new branch
gets the `podman-port` name.

## 13Nov2026

Replaced per-target build scripts with the registry (scripts/podman/targets.sh).
`build-spindle-*.sh` are now dead code; delete when convenient.

Old srun/rshlaunch test scripts reviewed (podman-port-old/scripts/podman/).
- test-spindle-slurm-srun.sh is the N-instance one; the "-parallel" one is stale.
- srun needs no generate_config.sh / mariadb.env (password passed at runtime).
- Do not copy blindly: exit status is lost through `| tee` (no pipefail),
  and there are /dev/tty prompts.
- ~100s of the runtime is fixed sleeps; replace with readiness polling.

Open question: does name-based container DNS work under rootless podman here?
(rshlaunch scripts inject /etc/hosts because aardvark-dns "did not function".)
The answer decides the cluster bring-up design.

Container-name DNS is broken on this system, and the cause is known.  netavark launches
aardvark-dns via `systemd-run -q --scope --user`, which fails because /run/user/<uid> (and its
bus) does not exist.  netavark ignores the failure; no DNS server runs.  IP routing is fine.
Started by hand inside the rootless netns, aardvark-dns serves names correctly and picks up
containers started later:
  podman unshare --rootless-netns aardvark-dns --config <runroot>/networks/aardvark-dns -p 53 run
Decision: cluster helper pre-starts aardvark-dns; /etc/hosts injection is the fallback.
Also: `podman inspect` cannot read IPs here (cgroup setup fails); read them from inside containers.
Evidence: scripts/podman/probe-dns.sh (output in .podman-logs/probe-dns-run3.txt).

Cluster bring-up works: DNS via pre-started aardvark-dns, readiness polling, 34s to ready.
The first Spindle test then hangs; srun reports "Requested nodes are busy" (step creation
disabled).  Hypothesis: Slurm 25.05 steps claim all CPUs, leaving none for Spindle's daemon step.
Not yet confirmed (needs a SPINDLE_DEBUG=3 log).

Podman gotcha: if /run/user/<uid> does not exist when containers are started, crun state goes to
<TMPDIR>/storage-run-<uid>.  A later login (e.g. a second terminal) creates /run/user/<uid>, and
exec/stop/rm on the running containers then fail ("crun: error opening file .../status").
Hypothesis, not verified.  Plan: pin XDG_RUNTIME_DIR in spindle-podman and the podman wrapper.

`slurm-srun` passes.

# Current Commit
[podman-port] slurm-rshlaunch target passes; one test script for all Slurm flavors

sshd needs uid/gid 65534, which rootless podman does not map by default, so
ssh logins were reset.  The target adds --uidmap/--gidmap for 65534 to the
Slurm containers (T_RUN_ARGS, from LLNL's container gotchas page).
`diagnose ssh` shows it: login fails by default, works with the maps.

Also: test-spindle-slurm.sh replaces the srun-only script; build guard shared
as check_launch_mode.sh.podman (checks srun or rsh mode).
