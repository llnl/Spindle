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

#include <pwd.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <fcntl.h>
#include <string>
#include <errno.h>

#include "ldcs_api.h"
#include "config.h"
#include "jobtask.h"
#include "launcher.h"
#include "parse_launcher.h"
#include "spindle_debug.h"
#include "spindle_launch.h"
#include "parse_launcher.h"
#include "spindle_session.h"
#include "config_mgr.h"

using namespace std;

static void setupLogging(int argc, char **argv);
static bool getNextTask(Launcher *launcher, spindle_args_t *params, vector<JobTask*> &tasks, const ConfigMap &config);

#if defined(HAVE_LMON)
extern Launcher *createLaunchmonLauncher(spindle_args_t *params, ConfigMap &config_);
#endif
extern Launcher *createSerialLauncher(spindle_args_t *params, ConfigMap &config_);
extern Launcher *createHostbinLauncher(spindle_args_t *params, ConfigMap &config_);
extern Launcher *createMPILauncher(spindle_args_t *params, ConfigMap &config_);
extern Launcher *createLSFLauncher(spindle_args_t *params, ConfigMap &config_);
extern Launcher *createPassthroughLauncher(spindle_args_t *params, ConfigMap &config_);

Launcher *newLauncher(spindle_args_t *params, ConfigMap &config)
{
   if (params->opts & OPT_OFF) {
      debug_printf("Turning spindle off. Using pass-through launcher\n");
      return createPassthroughLauncher(params, config);
   }
   if (params->use_launcher == serial_launcher) {
      debug_printf("Starting application in serial mode\n");
      return createSerialLauncher(params, config);
   }
   else if (params->startup_type == startup_lmon) {
      debug_printf("Starting application with launchmon\n");
#if defined(HAVE_LMON)
      return createLaunchmonLauncher(params, config);
#else
      fprintf(stderr, "Spindle Error: Spindle was not built with LaunchMON support\n");
      err_printf("HAVE_LMON not defined\n");
      return NULL;
#endif
   }
   else if (params->startup_type == startup_hostbin) {
      debug_printf("Starting application with hostbin\n");
      return createHostbinLauncher(params, config);
   }
   else if (params->startup_type == startup_mpi) {
      debug_printf("Starting application with MPI startup\n");
      return createMPILauncher(params, config);
   }
   else if (params->startup_type == startup_lsf) {
      debug_printf("Starting application with MPI startup\n");
      return createLSFLauncher(params, config);
   }
   err_printf("Mis-set use_launcher value: %d\n", (int) params->use_launcher);
   return NULL;
}

int main(int argc, char *argv[])
{
   bool result;
   setupLogging(argc, argv);

   ConfigMap config("[Spindle Config]");
   string errmessage;
   result = gatherAllConfigInfo(argc, argv, true, config, errmessage);
   if (!result) {
      fprintf(stderr, "Error: %s\n", errmessage.c_str());
      debug_printf("Aborting due to error parsing args or configs\n");
      return -1;
   }

   spindle_args_t *params = (spindle_args_t *) malloc(sizeof(spindle_args_t));;
   memset(params, 0, sizeof(spindle_args_t));
   result = config.toSpindleArgs(*params);
   if (!result) {
      debug_printf("Aborting due to internal error moving config to spindle_args_t\n");
      return -1;
   }
   result = config.getUniqueID(params->unique_id, errmessage);
   if (!result) {
      fprintf(stderr, "Error: %s\n", errmessage.c_str());
      return -1;
   }
   params->number = config.getNumber();

   Launcher *launcher = newLauncher(params, config);
   if (!launcher) {
      fprintf(stderr, "Internal error while initializing spindle\n");
      return -1;
   }
   
   init_session(params, config, launcher);

   debug_printf("Spawning spindle daemons\n");
   result = launcher->setupDaemons();
   if (!result) {
      fprintf(stderr, "Internal error while spawning spindle daemons\n");
      return -1;
   }

   int num_live_jobs = 0, num_run_jobs = 0;
   int nonzero_rc = 0;
   bool session_shutdown = false, do_shutdown = false, initialized_spindle = false;

   //Main loop when running multiple jobs
   for (;;) {
      vector<JobTask*> tasks;
      debug_printf("Getting next task in Spindle main loop\n");
      result = getNextTask(launcher, params, tasks, config);
      if (!result) {
         fprintf(stderr, "Internal error while getting next spindle job\n");
         return -1;
      }
      debug_printf2("Got %lu tasks.  Handling all\n", (unsigned long) tasks.size());
      
      for (vector<JobTask*>::iterator t = tasks.begin(); t != tasks.end(); t++) {
         JobTask *task = *t;
         debug_printf2("Handling task %s\n", task->task_str());
         
         if (task->isInit() && !initialized_spindle) {
            debug_printf("Calling spindleInitFE\n");
            const char **hosts = launcher->getProcessTable();
            int iresult = spindleInitFE(hosts, params);
            if (iresult == -1) {
               err_printf("spindleInitFE returned an error\n");
               launcher->handleShutdown();
               return -1;
            }
            debug_printf("Done calling spindleInitFE\n");
            initialized_spindle = true;            
            signal_init_done();
         }
         else if (task->isJobDone()) {
            debug_printf("Job completed\n");
            app_id_t appid = task->getJobDoneID();
            int rc = task->getReturnCode();
            if (params->opts & OPT_SESSION) 
               mark_session_job_done(appid, rc);
            else if (rc != 0 && nonzero_rc == 0)
               nonzero_rc = rc;
            num_live_jobs--;
         }
         else if (task->isLaunch()) {
            int app_argc = 0;
            char **app_argv = NULL;
            app_id_t id;
            task->getAppArgs(id, app_argc, app_argv);
            debug_printf("Running application with id %lu: ", id);
            for (int i = 0; i < app_argc; i++) {
               bare_printf("%s ", app_argv[i]);
            }
            bare_printf("\n");
            result = launcher->setupJob(id, app_argc, app_argv);
            if (!result) {
               debug_printf("Failed in caller to launcher::setupJob\n");
            }
            else {
               debug_printf2("Modified app argv to: ");
               for (int i = 0; i < app_argc; i++) {
                  bare_printf2("%s ", app_argv[i]);
               }
               bare_printf2("\n");
               if (params->opts & OPT_SESSION) {
                  debug_printf("Returning app cmdline to proc with session-id %lu for running\n", id);
                  result = (return_session_cmd(id, app_argc, app_argv) == 0);
                  num_run_jobs++;
               }
               else {
                  debug_printf("Spawning application for session-id %lu\n", id);
                  result = launcher->spawnJob(id, app_argc, app_argv);
                  num_live_jobs++;
                  num_run_jobs++;
                  debug_printf("num_live_jobs = %d, num_run_jobs = %d\n", num_live_jobs, num_run_jobs);
               }
            }
            if (!result) {
               debug_printf("Error launching session.  Marking it as done.\n");
               if (params->opts & OPT_SESSION) 
                  mark_session_job_done(id, -1);
            }
         }
         else if (task->isSessionShutdown()) {
            if (session_shutdown) {
               debug_printf("Second shutdown request.  Forcing shutdown despite num_live_jobs = %d\n", num_live_jobs);
               do_shutdown = true;
            }
            else {
               debug_printf("Shutdown requested for after current task\n");               
            }
            session_shutdown = true;            
         }
         else if (task->isDaemonDone()) {
            int rc = task->getReturnCode();
            if (rc != 0 && nonzero_rc == 0)
               nonzero_rc = rc;
            do_shutdown = true;
         }
         delete task;
      }

      if (session_shutdown && num_live_jobs == 0) {
         do_shutdown = true;
      }

      if ((num_run_jobs == 1 || do_shutdown) && !initialized_spindle) {
         debug_printf("Calling spindleInitFE\n");
         const char **hosts = launcher->getProcessTable();
         int iresult = spindleInitFE(hosts, params);
         if (iresult == -1) {
            err_printf("[LMON FE] spindleInitFE returned an error\n");
            launcher->handleShutdown();
            return -1;
         }
         debug_printf("Done calling spindleInitFE\n");
         signal_init_done();         
         initialized_spindle = true;
      }

      if (do_shutdown) {
         launcher->handleShutdown();
         spindleCloseFE(params);
         LOGGING_FINI;
         return nonzero_rc;
      }
   }
}

static void setupLogging(int argc, char **argv)
{
   LOGGING_INIT(const_cast<char *>("FE"));
   debug_printf("Spindle Command Line: ");
   for (int i = 0; i < argc; i++) {
      bare_printf("%s ", argv[i]);
   }
   bare_printf("\n");
}

static bool getNextTask(Launcher *launcher, spindle_args_t *params, vector<JobTask*> &tasks, const ConfigMap &config)
{
   int app_argc;
   char **app_argv;      
   static bool init_done = false;

   if (!(params->opts & OPT_SESSION) && !init_done) {
      debug_printf("Not in session mode, and running single job. Adding init/run/shutdown tasks to task queue\n");
      JobTask *init = new JobTask();
      init->setInit();
      tasks.push_back(init);
      
      JobTask *runapp = new JobTask();
      config.getApplicationCmdline(app_argc, app_argv);
      runapp->setLaunch(1, app_argc, app_argv);
      runapp->setNoClean();
      tasks.push_back(runapp);      

      JobTask *shutdown = new JobTask;
      shutdown->setSessionShutdown();
      tasks.push_back(shutdown);

      init_done = true;
      return true;
   }
   if (!init_done) {
      debug_printf("Adding init task to job queue\n");
      JobTask *init = new JobTask();
      init->setInit();
      tasks.push_back(init);
      init_done = true;
      return true;
   }

   int launcher_fd = launcher->getJobFinishFD();
   int session_fd = get_session_fd();
   int max_fd = session_fd > launcher_fd ? session_fd : launcher_fd;
   
   fd_set readset;
   FD_ZERO(&readset);
   if (launcher_fd != -1)
      FD_SET(launcher_fd, &readset);
   if (session_fd != -1)
      FD_SET(session_fd, &readset);
   assert(max_fd != -1);
   int result;
   do {
      result = select(max_fd+1, &readset, NULL, NULL, NULL);
   } while (result == -1 && errno == EINTR);
   if (result == -1) {
      int error = errno;
      err_printf("Could not wait on socket: %s\n", strerror(error));
      return false;
   }

   if (launcher_fd != -1 && FD_ISSET(launcher_fd, &readset)) {
      bool daemon_done = false;
      int daemon_ret = 0;
      vector<pair<app_id_t, int> > app_rets;
      result = launcher->getReturnCodes(daemon_done, daemon_ret, app_rets);
      if (!result) {
         debug_printf("Failed to get return codes from spindle.\n");
         return false;
      }
      
      if (daemon_done) {
         JobTask *task = new JobTask();
         task->setDaemonDone(daemon_ret);
         tasks.push_back(task);
      }
      for (vector<pair<app_id_t, int> >::iterator i = app_rets.begin(); i != app_rets.end(); i++) {
         JobTask *task = new JobTask();
         task->setJobDone(i->first, i->second);
         tasks.push_back(task);
      }

      char byte;
      do {
         result = read(launcher_fd, &byte, sizeof(byte));
      } while (result == -1 && errno == EINTR);
      if (result == -1) {
         int error = errno;
         err_printf("Could not clear byte from launcher socket: %s\n", strerror(error));
         return false;
      }
   }
   
   if (session_fd != -1 && FD_ISSET(session_fd, &readset)) {
      bool session_complete = false;
      app_id_t appid;
      result = get_session_runcmds(appid, app_argc, app_argv, session_complete);
      if (result == -1) {
         debug_printf("Error reading session command. Dropping.\n");
         return true;
      }
      if (session_complete) {
         JobTask *task = new JobTask();
         task->setSessionShutdown();
         tasks.push_back(task);
      }
      if (app_argc > 0) {
         JobTask *task = new JobTask();
         task->setLaunch(appid, app_argc, app_argv);
         tasks.push_back(task);         
      }
   }

   return true;
}
