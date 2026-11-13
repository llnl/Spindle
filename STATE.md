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


# Manifest (last updated 13Nov2026)

podman-port/
            .dockerignore
            spindle-podman                                                      primary interface, bash script.

podman-port/scripts/podman/
                           common.sh                                            has the LC cert mounts and the podman_build helper.
                           build-spindle-serial.sh                              build script for serial

podman-port/containers/spindle-serial-ubuntu/
                                             Dockerfile.podman

podman-port/containers/spindle-serial-ubuntu/scripts/
                                                     entrypoint.sh.podman


# State
    ✓ `spindle-podman status`
    ✓ `spindle-podman help`

# Diary

## 13Nov2026
Starting the fourth attempt...
In the previous `podman-port` branch I did a rebase that ended
up scrambling the hashes for subsequent commits.  I also failed to
keep track of files associated with earlier attempts, newer files,
debugging work, etc.  Today's task is to start at the top with the
`spindle-podman` script and just pull over what it needs.  The
previous branch has been renamed `podman-port-old`.  The new branch
gets the `podman-port` name.

Works:  ./spindle-podman build serial

Bumped baseline build time to 450s.

# Current Commit
[podman-port] Bump baseline serial build time to 450s

The check is here because of an earlier bug that kept adding
build results to the same tarball; it eventually exceeded
96 GB.  Slow build times were the first tip-off that something
was amiss.

