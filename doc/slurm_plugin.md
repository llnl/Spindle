Spindle Slurm plugin
====================

The Spindle Slurm plugin integrates Spindle into Slurm through the
SPANK interface as an alternative launch mechanism to the srun wrapper.
It adds the ability to launch job steps using `srun --spindle`.

## Building and configuring the plugin

Configure Spindle with `--enable-slurm-plugin`:

```bash
./configure --with-rm=slurm-plugin --enable-slurm-plugin [--with-slurm-dir=/path/to/slurm] ...
make
make install
```

Refer to `INSTALL` for more details on configuring Spindle.

After installation of Spindle, the plugin is installed at
`$PREFIX/lib/libspindleslurm.so`. It is registered with Slurm by adding the
following line to `/etc/slurm/plugstack.conf`:

```
required /path/to/spindle/lib/libspindleslurm.so
```

## Session launch modes

The manner in which Spindle sessions are started varies depending on 
the configuration of Spindle and of Slurm. 

When starting a session, the plugin must arrange for Spindle to start
on each compute node before any step runs within the allocation.
The most straightforward way to do this is to configure the cluster
to run job prologs at allocation time.  If your `slurm.conf` includes 
`PrologFlags=Alloc` (or another flag that implies it: `Contain`, 
`RunInJob`, `X11`, `ForceRequeueOnFail`, or `NoHold`), then sessions
will be started on each node of the allocation at the time the allocation
is made. 

If `PrologFlags=Alloc` or a related setting is *not* used, one of two
mechanisms is used to start the job on every node:

**RSH launch**: Spindle can use RSH/SSH to launch daemons from the
frontend (FE) process. To use the RSH launch mode, the cluster must be configured
such that passwordless ssh can be used to run commands on every compute 
node within the allocation without any interactive user input.
This mode is enabled by configuring Spindle with:

```bash
./configure --with-rm=slurm-plugin --enable-slurm-plugin --with-rsh-launch [--with-rsh-cmd=/usr/bin/ssh] ...
```

**Dummy srun fallback**: If neither `PrologFlags=Alloc` nor RSH launch is available,
Spindle will fall back on using a dummy `srun` invocation to force the prolog
to run on every compute node of the allocation. Note that this has the side-effect
of consuming step 0, so that the user's first step will instead be numbered 1.

## Using Spindle through the Slurm plugin

### Per-step mode: `--spindle`

Add `--spindle` to any `srun` command to use Spindle for that step.
Spindle daemons start before the application runs and shut down when
the step finishes.

```bash
srun --spindle ./my_application
```

Additional arguments can be passed to Spindle as an optional value of the argument `--spindle`:

```bash
srun --spindle="--level=low" ./my_application
```

### Session mode: `--spindle-session`

Session mode shares a Spindle session across multiple steps.
The use of sessions in the Slurm plugin differs from its use with
the other launchers. Unlike the other launchers, sessions are *not*
started with `spindle --start-session`. Rather, an additional argument
`--spindle-session` is added to `salloc` and `sbatch`. 

To use a session, include `--spindle-session` when creating the allocation:

```bash
salloc --spindle-session ...
```

Then run steps with `--spindle`:

```bash
srun --spindle ./app1
srun --spindle ./app2
srun --spindle ./app3
```

All steps within the allocation will run in the same Spindle session.
When the allocation exits, the session will terminate automatically.

Sessions can be used with an `sbatch` script as shown below:

```bash
#!/bin/bash
#SBATCH --spindle-session
#SBATCH -N 4
#SBATCH -n 4

srun --spindle ./app1
srun --spindle ./app2
srun --spindle ./app3
```

