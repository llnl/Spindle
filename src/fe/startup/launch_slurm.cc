/*
This file is part of Spindle.  For copyright information see the COPYRIGHT 
file in the top level directory, or at 
https://github.com/hpc/Spindle/blob/master/COPYRIGHT

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License (as published by the Free Software
Foundation) version 2.1 dated February 1999.  This program is distributed in the
hope that it will be useful, but WITHOUT ANY WARRANTY; without even the IMPLIED
WARRANTY OF MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the terms 
and conditions of the GNU Lesser General Public License for more details.  You should 
have received a copy of the GNU Lesser General Public License along with this 
program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "config.h"
#include "launcher.h"
#include "spindle_debug.h"

#include <string>
#include <vector>
#include <utility>
#include <map>
#include <set>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>

#include <unistd.h>

using namespace std;

class SlurmLauncher : public ForkLauncher
{
   friend Launcher *createSlurmLauncher(spindle_args_t *params, ConfigMap &config_);
private:
   int nnodes;
   bool initError;
   bool useRSH;
   set<char *> hostset;
   static SlurmLauncher *slauncher;
protected:
   virtual bool spawnDaemon();
   virtual bool spawnJob(app_id_t id, int app_argc, char **app_argv);
public:
   SlurmLauncher(spindle_args_t *params_, ConfigMap &config_);
   virtual ~SlurmLauncher();
   virtual const char **getProcessTable();
   virtual const char *getDaemonArg();
   virtual void getSecondaryDaemonArgs(vector<const char *> &secondary_args);
   virtual bool getReturnCodes(bool &daemon_done, int &daemon_ret,
                               std::vector<std::pair<app_id_t, int> > &app_rets);
};

SlurmLauncher *SlurmLauncher::slauncher = NULL;

Launcher *createSlurmLauncher(spindle_args_t *params, ConfigMap &config)
{
   assert(!SlurmLauncher::slauncher);
   SlurmLauncher::slauncher = new SlurmLauncher(params, config);
   if (SlurmLauncher::slauncher->initError) {
      delete SlurmLauncher::slauncher;
      SlurmLauncher::slauncher = NULL;
   }
   return SlurmLauncher::slauncher;
}

SlurmLauncher::SlurmLauncher(spindle_args_t *params_, ConfigMap &config_) :
   ForkLauncher(params_, config_),
   nnodes(0),
   initError(false)
{
   useRSH = params_->opts & OPT_RSHLAUNCH;
   params_->opts |= OPT_PERSIST;

   char *nodelist_str = getenv("SLURM_NODELIST");
   if (!nodelist_str)
      nodelist_str = getenv("SLURM_JOB_NODLIST");
   if (!nodelist_str) {
      fprintf(stderr, "ERROR: Spindle could not find the SLURM_NODELIST environment variable. "
              "Please run spindle from an already-allocated session.\n");
      err_printf("Could not find SLURM_NODELIST\n");
      initError = true;
      return;
   }
   char *nnodes_str = getenv("SLURM_NNODES");
   if (!nnodes_str)
      nnodes_str = getenv("SLURM_JOB_NUM_NODES");
   if (!nnodes_str) {
      fprintf(stderr, "ERROR: Spindle could not find the SLURM_NNODES environment variable. "
              "Please run spindle from an already-allocated session.\n");
      err_printf("Could not find SLURM_NNODES\n");
      initError = true;
      return;
   }   
   nnodes = atoi(nnodes_str);
   if (nnodes <= 0) {
      fprintf(stderr, "ERROR: Spindle could not parse the SLURM_NNODES environment variable. "
              "Please run spindle from an already-allocated session.\n");
      err_printf("Could not parse SLURM_NNODES\n");
      initError = true;      
   }

   size_t buffer_size = strlen(nodelist_str) + 64;
   char *buffer = (char *) malloc(buffer_size);
   
   snprintf(buffer, buffer_size, "scontrol show hostname \"%s\"", nodelist_str);
   FILE *f = popen(buffer, "r");
   if (!f) {
      int error = errno;
      err_printf("Failed to popen scontrol: %s\n", strerror(error));
      free(buffer);
      initError = true;
      return;
   }

   while (!feof(f)) {
      char *hostname = NULL;
      (void)! fscanf(f, "%ms", &hostname);
      if (hostname && *hostname)
         hostset.insert(hostname);
   }
   int result = pclose(f);
   if (result != 0) {
      int error = errno;
      fprintf(stderr, "Spindle encountered an error fetching the hostlist from scontrol. "
              "We tried to run the command:\n  %s\n", buffer);
      err_printf("scontrol returned %d: %s\n", result, strerror(error));
      free(buffer);
      initError = true;
      return;
   }
   free(buffer);
}

SlurmLauncher::~SlurmLauncher()
{
   slauncher = NULL;
}

bool SlurmLauncher::spawnDaemon()
{
   if (useRSH) {
      debug_printf("RSH launch mode enabled.  Slurm launcher not manually spawning daemons\n");
      return true;
   }
   
   daemon_pid = fork();
   if (daemon_pid == -1) {
      err_printf("Failed to fork process for daemon: %s\n", strerror(errno));
      return false;
   }
   else if (daemon_pid == 0) {
      int total_args = 10 + daemon_argc;
      char **new_daemon_args = (char **) malloc(total_args * sizeof(char *));
      int i = 0;
      char count_buffer[64];
      snprintf(count_buffer, 64, "%d", nnodes);
      new_daemon_args[i++] = const_cast<char *>("srun");
#if HAVE_SRUN_EXTERNAL_LAUNCHER
      new_daemon_args[i++] = const_cast<char *>("--external-launcher");
      new_daemon_args[i++] = const_cast<char *>("--wait=0");
#else
      new_daemon_args[i++] = const_cast<char *>("--ntasks-per-node=1");
      new_daemon_args[i++] = const_cast<char *>("--wait=0");
      new_daemon_args[i++] = const_cast<char *>("--gres=none");
      new_daemon_args[i++] = const_cast<char *>("--mem=0");
#endif
      new_daemon_args[i++] = const_cast<char *>("-n");
      new_daemon_args[i++] = count_buffer;
      new_daemon_args[i++] = const_cast<char *>("-N");
      new_daemon_args[i++] = count_buffer;
      for (int j = 0; j < daemon_argc; j++)
         new_daemon_args[i++] = daemon_argv[j];
      new_daemon_args[i++] = NULL;
      assert(i <= total_args);
      debug_printf("Execing daemon in pid %d with command line: ", getpid());
      for (i = 0; new_daemon_args[i]; i++) {
         bare_printf("%s ", new_daemon_args[i]);
      }
      bare_printf("\n");

      execvp(new_daemon_args[0], new_daemon_args);

      int error = errno;
      err_printf("Could not exec srun of daemon: %s\n", strerror(error));
      fprintf(stderr, "Error launching spindle daemon via srun: %s\n", strerror(error));
      fprintf(stderr, "Attempted command line was: ");
      for (i = 0; new_daemon_args[i]; i++) {
         fprintf(stderr, "%s ", new_daemon_args[i]);
      }
      fprintf(stderr, "\n");
      exit(-1);
   }

   return true;
}

bool SlurmLauncher::spawnJob(app_id_t id, int app_argc, char **app_argv)
{
   (void)app_argc;
   debug_printf("Spindle launching slurm job with app-id %lu: %s\n", id, app_argv[0]);
   int pid = fork();
   if (pid == -1) {
      int error = errno;
      err_printf("Failed to fork process for app srun: %s\n", strerror(error));
      return false;
   }
   else if (pid == 0) {
      // Inject --export to propagate SPINDLE_DEBUG and SPINDLE_TEST to remote nodes
      // Build new argv with --export=SPINDLE_DEBUG,SPINDLE_TEST,TMPDIR,TEMPDIR inserted after srun
      int new_argc = app_argc + 1;
      char **new_argv = (char **) malloc((new_argc + 1) * sizeof(char *));
      new_argv[0] = app_argv[0];  // srun
      new_argv[1] = const_cast<char *>("--export=SPINDLE_DEBUG,SPINDLE_TEST,TMPDIR,TEMPDIR");
      for (int i = 1; i < app_argc; i++) {
         new_argv[i + 1] = app_argv[i];
      }
      new_argv[new_argc] = NULL;

      debug_printf("Execing srun with --export: ");
      for (int i = 0; new_argv[i]; i++) {
         bare_printf("%s ", new_argv[i]);
      }
      bare_printf("\n");

      execvp(*new_argv, new_argv);
      int error = errno;
      fprintf(stderr, "Spindle failed to run %s: %s\n", new_argv[0], strerror(error));
      err_printf("Failed to run application %s: %s\n", new_argv[0], strerror(error));
      exit(-1);
   }
   app_pids[pid] = id;
   return true;
}

static int cmpstr(const void *a, const void *b)
{
   return strcmp(*(const char **) a, *(const char **) b);
}

const char **SlurmLauncher::getProcessTable()
{
   char **proctable = (char **) malloc(sizeof(char*) * (hostset.size()+1));
   int j = 0;
   for (set<char *>::iterator i = hostset.begin(); i != hostset.end(); i++, j++) {
      proctable[j] = *i;
      debug_printf2("Adding host %s to proctable[%d]\n", proctable[j], j);
   }
   proctable[j] = NULL;
   qsort(proctable, j, sizeof(char *), cmpstr);
   return const_cast<const char **>(proctable);
}

const char *SlurmLauncher::getDaemonArg()
{
   return "--spindle_mpi";
}

void SlurmLauncher::getSecondaryDaemonArgs(vector<const char *> &secondary_args)
{
   if (useRSH)
      return;
   
   char port_str[32], ss_str[32], port_num_str[32];
   snprintf(port_str, 32, "%d", params->port);
   snprintf(port_num_str, 32, "%d", params->num_ports);
   snprintf(ss_str, 32, "%lu", params->unique_id);
   secondary_args.push_back(strdup(port_str));
   secondary_args.push_back(strdup(port_num_str));
   secondary_args.push_back(strdup(ss_str));
}

bool SlurmLauncher::getReturnCodes(bool &daemon_done, int &daemon_ret,
                                   std::vector<std::pair<app_id_t, int> > &app_rets)
{   
   if (!daemon_pid && useRSH) {
      daemon_pid = getRSHPidFE();
      debug_printf3("Marked daemon pid as %d\n", (int) daemon_pid);
      markRSHPidReapedFE();
   }
   return ForkLauncher::getReturnCodes(daemon_done, daemon_ret, app_rets);
}
