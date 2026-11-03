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

#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "spindle_debug.h"
#include "ldcs_api.h"
#include "spindle_launch.h"
#include "client.h"
#include "client_api.h"
#include "exec_util.h"
#include "shmcache.h"
#include "parseloc.h"
#include "client_libraries.h"
#include "config.h"

#if !defined(LIBEXECDIR)
#error Expected to be built with libdir defined
#endif
#if !defined(PROGLIBDIR)
#error Expected to be built with proglib defined
#endif

char spindle_daemon[] = LIBEXECDIR "/spindle_be";
char spindle_interceptlib[] = PROGLIBDIR "/libspindleint.so";

int ldcsid = -1;
unsigned int shm_cachesize;

static int rankinfo[4]={-1,-1,-1,-1};
number_t number;
static int use_cache;
static unsigned int cachesize;
static char *commpath, *number_s, *commpaths;
static char **cmdline;
static char *executable;
static char *client_lib;
static char *opts_s;
static char **daemon_args;
static char *cachesize_s;
static char *executable_abspath;

opt_t opts;

extern int spindle_mkdir(char *path);
extern char *parse_location(char *loc, number_t number);
extern char *realize(char *path);

static int establish_connection()
{
   debug_printf2("Opening connection to server\n");
   ldcsid = client_open_connection(commpath, number);
   if (ldcsid == -1) 
      return -1;

   send_pid(ldcsid);
   if (send_rankinfo_query(ldcsid, &rankinfo[0], &rankinfo[1], &rankinfo[2], &rankinfo[3]) == -1) {
      err_printf("Failed to get rank info from server\n");
      return -1;
   }
   if (opts & OPT_NUMA)
      send_cpu(ldcsid, get_cur_cpu());

   return 0;
}

static int setup_environment()
{
   char rankinfo_str[256];
   snprintf(rankinfo_str, 256, "%d %d %d %d %d", ldcsid, rankinfo[0], rankinfo[1], rankinfo[2], rankinfo[3]);

   char *connection_str = NULL;
   if (opts & OPT_RELOCAOUT) 
      connection_str = client_get_connection_string(ldcsid);

   char *chosen_parsed_cachepath = NULL;
   int rc = send_cachepath_query( ldcsid , NULL, &chosen_parsed_cachepath);
   if( 0 != rc ){
       return rc;
   }

   setenv("LD_AUDIT", client_lib, 1);
   setenv("LDCS_COMMPATH", commpath, 1);
   setenv("LDCS_CHOSEN_PARSED_CACHEPATH", chosen_parsed_cachepath, 1);
   setenv("LDCS_NUMBER", number_s, 1);
   setenv("LDCS_RANKINFO", rankinfo_str, 1);
   if (connection_str)
      setenv("LDCS_CONNECTION", connection_str, 1);
   setenv("LDCS_OPTIONS", opts_s, 1);
   setenv("LDCS_CACHESIZE", cachesize_s, 1);
   setenv("LDCS_BOOTSTRAPPED", "1", 1);
   setenv("SPINDLE", "true", 1);
   if (opts & OPT_SUBAUDIT) {
      char *preload_str = spindle_interceptlib;
      char *preload_env = getenv("LD_PRELOAD");
      char *preload;
      if (preload_env) {
         size_t len = strlen(preload_env) + strlen(preload_str) + 2;
         preload = malloc(len);
         snprintf(preload, len, "%s %s", preload_str, preload_env);
      }
      else {
         preload = preload_str;
      }
      setenv("LD_PRELOAD", preload, 1);
      
      if (preload_env) {
         free(preload);
      }
   }
   return 0;
}

static int parse_cmdline(int argc, char *argv[])
{
   int i, daemon_arg_count;
   if (argc < 5)
      return -1;
   
   i = 1;
   if (strcmp(argv[1], "-daemon_args") == 0) {
      debug_printf("Parsing daemon args out of bootstrap launch line\n");
      daemon_arg_count = atoi(argv[2]);
      daemon_args = malloc(sizeof(char *) * (daemon_arg_count + 2));
      daemon_args[0] = spindle_daemon;
      for (i = 4; i < 3 + daemon_arg_count; i++)
         daemon_args[i - 3] = argv[i];
      daemon_args[i - 3] = NULL;
   }

   commpaths = argv[i++];
   i++; // Skip over candidate_cachepaths.
   number_s = argv[i++];
   number = (number_t) strtoul(number_s, NULL, 0);
   opts_s = argv[i++];
   opts = strtoul(opts_s, NULL, 10);
   cachesize_s = argv[i++];
   cachesize = atoi(cachesize_s);
   cmdline = argv + i;
   assert(i < argc);

   return 0;
}

static void launch_daemon( void )
{
   /*grand-child fork, then execv daemon.  By grand-child forking we ensure that
     the app won't get confused by seeing an unknown process as a child. */
   pid_t child, gchild;
   int status, result;
   int fd;
   char unique_file[MAX_PATH_LEN+1];
   char buffer[32];

   snprintf(unique_file, MAX_PATH_LEN, "%s/spindle_daemon_pid", commpath);
   unique_file[MAX_PATH_LEN] = '\0';
   fd = open(unique_file, O_CREAT | O_EXCL | O_WRONLY, 0600);
   if (fd == -1) {
      debug_printf("Not starting daemon -- %s already exists\n", unique_file);
      return;
   }
   debug_printf("Client is spawning daemon\n");
      
   child = fork();
   if (child == 0) {
      gchild = fork();
      if (gchild != 0) {
         snprintf(buffer, sizeof(buffer), "%d\n", getpid());
         (void)! write(fd, buffer, strlen(buffer));
         close(fd);
         exit(0);
      }
      close(fd);
      result = setpgid(0, 0);
      if (result == -1) {
         err_printf("Failed to setpgid: %s\n", strerror(errno));
      }
      execv(spindle_daemon, daemon_args);
      fprintf(stderr, "Spindle error: Could not execv daemon %s\n", daemon_args[0]);
      exit(-1);
   }
   else if (child > 0) {
      close(fd);
      waitpid(child, &status, 0);
   }
}

static void get_executable()
{
   int errcode = 0;
   char *glibc_tunables_env_value = NULL;
   char *path_component = NULL;

   if (!(opts & OPT_RELOCAOUT) || (opts & OPT_REMAPEXEC)) {
      debug_printf3("Using default executable %s\n", *cmdline);
      executable = *cmdline;
      path_component = find_path_component(ldcsid, *cmdline, NULL);
      calc_static_tls(*cmdline, path_component, NULL, &glibc_tunables_env_value, NULL);
      if (glibc_tunables_env_value) {
         debug_printf2("Setting GLIBC_TUNABLES to '%s' for %s to fix static tls\n", glibc_tunables_env_value, *cmdline);
         setenv("GLIBC_TUNABLES", glibc_tunables_env_value, 1);
      }
      if (path_component) {
         free(path_component);
         path_component = NULL;
      }
      return;
   }

   debug_printf2("Sending request for executable %s\n", *cmdline);
   exec_pathsearch(ldcsid, *cmdline, &executable, &errcode, &executable_abspath, &path_component);

   if (executable == NULL) {
      executable = *cmdline;
      err_printf("Failed to relocate executable %s\n", executable);
   }
   else {
      debug_printf("Relocated executable %s to %s\n", *cmdline, executable);
      chmod(executable, 0700);
   }

   calc_static_tls(*cmdline, path_component, NULL, &glibc_tunables_env_value, NULL);
   if (glibc_tunables_env_value) {
      debug_printf2("Setting GLIBC_TUNABLES to '%s' for %s to fix static tls\n", glibc_tunables_env_value, *cmdline);
      setenv("GLIBC_TUNABLES", glibc_tunables_env_value, 1);
   }

   if (path_component) {
      free(path_component);
   }
}

static void adjust_script()
{
   int result;
   char **new_cmdline;
   char *new_executable;

   if (!(opts & OPT_RELOCAOUT)) {
      return;
   }

   if (!executable)
      return;

   result = adjust_if_script(*cmdline, executable, cmdline, &new_executable, &new_cmdline, executable_abspath);
   if (result != 0)
      return;

   cmdline = new_cmdline;
   executable = new_executable;
}

static void get_clientlib()
{
   char *default_libstr = (opts & OPT_SUBAUDIT) ? default_subaudit_libstr : default_audit_libstr;
   int errorcode;
   
   get_relocated_file(ldcsid, default_libstr, 1, &client_lib, &errorcode, NULL);
   if (client_lib == NULL) {
      client_lib = default_libstr;
      err_printf("Failed to relocate client library %s\n", default_libstr);
   }
   else {
      debug_printf2("Relocated client library %s to %s\n", default_libstr, client_lib);
      chmod(client_lib, 0600);
   }
}

void test_log(const char *name)
{
   (void)name;
}

static int handle_exec_failure(char **cmdline, int errno_val)
{
   int result;
   int one_per_job;
   const char *errmsg;
   const char *executable;
   errmsg = strerror(errno_val);
   executable = cmdline && cmdline[0] ? cmdline[0] : "[NULL]";
   if (ldcsid == -1) {
      debug_printf("Exec failure on %s without server setup: %s\n", executable, errmsg);
      fprintf(stderr, "%s: %s\n", executable, errmsg);
      return -1;
   }

   result = send_pickone_query(ldcsid, "bootstrap exec failure", &one_per_job);
   if (result == -1) {
      debug_printf("Exec and pickone failure on %s: %s\n", executable, errmsg);
      fprintf(stderr, "%s: %s\n", executable, errmsg);
      return -1;
   }

   if (one_per_job) {
      debug_printf("Exec failure on %s: %s\n", executable, errmsg);
      fprintf(stderr, "%s: %s\n", executable, errmsg);
      return -1;
   }
   return -1;
}

/**
 * Are you staring at this strange code after running TotalView (or another debugger)?
 *
 * If so, you're currently stopped at spindle's bootstrapper, which is providing scalable program launch on this system. 
 * Go ahead and hit 'go'. Your regularly scheduled program will resume shortly. 
 **/
int main(int argc, char *argv[])
{
   int result;
   char **j, *spindle_env;

   LOGGING_INIT_PREEXEC("Client");
   debug_printf("Launched Spindle Bootstrapper\n");

   result = parse_cmdline(argc, argv);
   if (result == -1) {
      fprintf(stderr, "spindle_boostrap cannot be invoked directly\n");
      return -1;
   }

   spindle_env = getenv("SPINDLE");
   if (spindle_env) {
      if (strcasecmp(spindle_env, "false") == 0 || strcmp(spindle_env, "0") == 0) {
         debug_printf("Turning off spindle from bootstrapper because SPINDLE is %s\n", spindle_env);
         execvp(cmdline[0], cmdline);
         return handle_exec_failure(cmdline, errno);
      }
   }

   if( -1 == getFirstValidPath( commpaths, &commpath, number ) ){
       return -1;
   }

   if (daemon_args) {
      launch_daemon();
   }
   
   result = establish_connection();
   if (result == -1) {
      err_printf("spindle_bootstrap failed to connect to daemons\n");
      return -1;
   }

   if (isExecExcluded(cmdline[0])) {
      debug_printf("Turning off spindle because we're running an excluded binary: %s\n", cmdline[0]);
      execvp(cmdline[0], cmdline);
      return handle_exec_failure(cmdline, errno);
   }


   if ((opts & OPT_SHMCACHE) && cachesize) {
      unsigned int shm_cache_limit;
      cachesize *= 1024;
#if defined(COMM_BITER)
      shm_cache_limit = cachesize > 512*1024 ? cachesize - 512*1024 : 0;
#else
      shm_cache_limit = cachesize;
#endif
      shmcache_init(commpath, number, cachesize, shm_cache_limit);
      use_cache = 1;
   }      
   
   get_executable();
   get_clientlib();
   adjust_script();
   
   /**
    * Exec setup
    **/
   debug_printf("Spindle bootstrap launching: ");
   if (!executable) {
      bare_printf("<no executable given>");
   }
   else {
      bare_printf("%s.  Args:  ", executable);
      for (j = cmdline; *j; j++) {
         bare_printf("%s ", *j);
      }
   }
   bare_printf("\n");

   /**
    * Exec the user's application.
    **/
   if (setup_environment() != 0) {
      err_printf("spindle_bootstrap could not get the cache path from the server\n");
      return -1;
   }
   execvp(executable, cmdline);
   

   /**
    * Exec error handling
    **/
   return handle_exec_failure(cmdline, errno);
}
