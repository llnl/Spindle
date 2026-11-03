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

#include "parse_preload.h"
#include "ldcs_api.h"
#include "spindle_launch.h"
#include "fe_comm.h"
#include "keyfile.h"
#include "config.h"
#include "spindle_debug.h"
#include "ldcs_cobo.h"
#include "rshlaunch.h"
#include "config_mgr.h"

#include <string>
#include <cassert>
#include <pwd.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

#include <sstream>
#if defined(USAGE_LOGGING_FILE)
static const char *logging_file = USAGE_LOGGING_FILE;
#else
static const char *logging_file = NULL;
#endif
static const char spindle_bootstrap[] = LIBEXECDIR "/spindle_bootstrap";
static bool sendAndWaitForAlive();

using namespace std;

template<typename T>
void pack_param(T value, char *buffer, unsigned int &pos)
{
   memcpy(buffer + pos, &value, sizeof(T));
   pos += sizeof(T);
}

template<>
void pack_param<char*>(char *value, char *buffer, unsigned int &pos)
{
   if (value == NULL) {
      value = const_cast<char*>("");
   }
   unsigned int strsize = strlen(value) + 1;
   memcpy(buffer + pos, value, strsize);
   pos += strsize;
}

static int pack_data(spindle_args_t *args, void* &buffer, unsigned &buffer_size)
{  
   buffer_size = sizeof(unsigned int) * 7;
   buffer_size += sizeof(number_t);
   buffer_size += sizeof(opt_t);
   buffer_size += sizeof(unique_id_t);
   buffer_size += args->commpaths ? strlen(args->commpaths) + 1 : 1;
   buffer_size += args->candidate_cachepaths ? strlen(args->candidate_cachepaths) + 1 : 1;
   buffer_size += args->pythonprefix ? strlen(args->pythonprefix) + 1 : 1;
   buffer_size += args->preloadfile ? strlen(args->preloadfile) + 1 : 1;
   buffer_size += args->numa_files ? strlen(args->numa_files) + 1 : 1;
   buffer_size += args->numa_excludes ? strlen(args->numa_excludes) + 1 : 1;
   buffer_size += args->rsh_command ? strlen(args->rsh_command) + 1 : 1;
   buffer_size += args->local_prefixes ? strlen(args->local_prefixes) + 1 : 1;
   buffer_size += args->session_key ? strlen(args->session_key) + 1 : 1;
   buffer_size += args->exec_excludes ? strlen(args->exec_excludes) + 1 : 1;
   buffer_size += args->crash_log ? strlen(args->crash_log) + 1 : 1;

   unsigned int pos = 0;
   char *buf = (char *) malloc(buffer_size);
   pack_param(args->number, buf, pos);
   pack_param(args->port, buf, pos);
   pack_param(args->num_ports, buf, pos);
   pack_param(args->opts, buf, pos);
   pack_param(args->unique_id, buf, pos);
   pack_param(args->use_launcher, buf, pos);
   pack_param(args->startup_type, buf, pos);
   pack_param(args->shm_cache_size, buf, pos);
   pack_param(args->commpaths, buf, pos);
   pack_param(args->candidate_cachepaths, buf, pos);
   pack_param(args->pythonprefix, buf, pos);
   pack_param(args->preloadfile, buf, pos);
   pack_param(args->bundle_timeout_ms, buf, pos);
   pack_param(args->bundle_cachesize_kb, buf, pos);
   pack_param(args->numa_files, buf, pos);
   pack_param(args->numa_excludes, buf, pos);
   pack_param(args->rsh_command, buf, pos);
   pack_param(args->local_prefixes, buf, pos);
   pack_param(args->session_key, buf, pos);
   pack_param(args->exec_excludes, buf, pos);
   pack_param(args->crash_log, buf, pos);
   assert(pos == buffer_size);

   buffer = (void *) buf;
   return 0;
}

static void logUser()
{
   /* Collect username */
   char *username = NULL;
   if (getenv("USER")) {
      username = getenv("USER");
   }
   if (!username) {
      struct passwd *pw = getpwuid(getuid());
      if (pw) {
         username = pw->pw_name;
      }
   }
   if (!username) {
      username = getlogin();
   }
   if (!username) {
      err_printf("Could not get username for logging\n");
   }
      
   /* Collect time */
   struct timeval tv;
   gettimeofday(&tv, NULL);
   struct tm *lt = localtime(& tv.tv_sec);
   char time_str[256];
   time_str[0] = '\0';
   strftime(time_str, sizeof(time_str), "%c", lt);
   time_str[255] = '\0';
   
   /* Collect version */
   const char *version = VERSION;

   /* Collect hostname */
   char hostname[256];
   hostname[0] = '\0';
   gethostname(hostname, sizeof(hostname));
   hostname[255] = '\0';

   string log_message = string(username) + " ran Spindle v" + version + 
                        " at " + time_str + " on " + hostname;
   debug_printf("Logging usage: %s\n", log_message.c_str());

   FILE *f = fopen(logging_file, "a");
   if (!f) {
      err_printf("Could not open logging file %s\n", logging_file ? logging_file : "NONE");
      return;
   }
   fprintf(f, "%s\n", log_message.c_str());
   fclose(f);
}

static void setupSecurity(spindle_args_t *params)
{
   handshake_protocol_t handshake;
   int result;
   /* Setup security */
   switch (OPT_GET_SEC(params->opts)) {
      case OPT_SEC_MUNGE:
         debug_printf("Initializing FE with munge-based security\n");
         handshake.mechanism = hs_munge;
         break;
      case OPT_SEC_KEYFILE: {
         char *path;
         int len;
         debug_printf("Initializing FE with keyfile-based security\n");
         create_keyfile(params->unique_id);
         len = MAX_PATH_LEN+1;
         path = (char *) malloc(len);
         get_keyfile_path(path, len, params->unique_id);
         handshake.mechanism = hs_key_in_file;
         handshake.data.key_in_file.key_filepath = path;
         handshake.data.key_in_file.key_length_bytes = KEY_SIZE_BYTES;
         break;
      }
      case OPT_SEC_KEYLMON:
         debug_printf("Initializing BE with launchmon-based security\n");
         handshake.mechanism = hs_explicit_key;
         fprintf(stderr, "Error, launchmon based keys not yet implemented\n");
         exit(-1);
         break;
      case OPT_SEC_NULL:
         debug_printf("Initializing BE with NULL security\n");
         handshake.mechanism = hs_none;
         break;
   }
   result = initialize_handshake_security(&handshake);
   if (result == -1) {
      err_printf("Could not initialize security\n");
      exit(-1);
   }
}

int getApplicationArgsFE(spindle_args_t *params, int *spindle_argc, char ***spindle_argv)
{
   char number_s[32], opt_s[64], cachesize_s[32], security_s[32];
   char port_s[32], numports_s[32], uniqueid_s[32], daemonargc_s[32];
   int n = 0;

   LOGGING_INIT(const_cast<char *>("FE"));
      
   snprintf(number_s, sizeof(number_s), "%lu", (unsigned long) params->number);
   snprintf(opt_s, sizeof(opt_s), "%lu", (unsigned long) params->opts);
   snprintf(cachesize_s, sizeof(cachesize_s), "%u", params->shm_cache_size);
   snprintf(security_s, sizeof(security_s), "%u", (unsigned int) OPT_GET_SEC(params->opts));
   snprintf(port_s, sizeof(port_s), "%u", params->port);
   snprintf(numports_s, sizeof(numports_s), "%u", params->num_ports);
   snprintf(uniqueid_s, sizeof(uniqueid_s), "%lu", params->unique_id);
   snprintf(daemonargc_s, sizeof(daemonargc_s), "6");
   
   #define MAX_ARGS 16
   *spindle_argv = (char **) malloc(sizeof(char*) * MAX_ARGS);
   (*spindle_argv)[n++] = strdup(spindle_bootstrap);
   if (params->opts & OPT_SELFLAUNCH) {
      (*spindle_argv)[n++] = strdup("-daemon_args");
      (*spindle_argv)[n++] = strdup(daemonargc_s);
      (*spindle_argv)[n++] = strdup("--spindle_selflaunch");
      (*spindle_argv)[n++] = strdup(security_s);
      (*spindle_argv)[n++] = strdup(number_s);
      (*spindle_argv)[n++] = strdup(port_s);
      (*spindle_argv)[n++] = strdup(numports_s);
      (*spindle_argv)[n++] = strdup(uniqueid_s);
   }
   (*spindle_argv)[n++] = strdup(params->commpaths);
   (*spindle_argv)[n++] = strdup(params->candidate_cachepaths);
   (*spindle_argv)[n++] = strdup(number_s);
   (*spindle_argv)[n++] = strdup(opt_s);
   (*spindle_argv)[n++] = strdup(cachesize_s);
   (*spindle_argv)[n] = NULL;
   *spindle_argc = n;
   assert(n < MAX_ARGS);

   return 0;
}

void fillInSpindleArgsFE(spindle_args_t *params)
{
   fillInSpindleArgsCmdlineFE(params, 0, 0, NULL, NULL);
}

static string fillargs_errmsg;
int fillInSpindleArgsCmdlineFE(spindle_args_t *params, unsigned int options, int sargc, char *sargv[], char **errstr)
{
   LOGGING_INIT(const_cast<char *>("FE"));
   static ConfigMap config("[Launcher Default Config]");

   int i = 0;
   int mod_argc = sargc + 2;
   char **mod_argv = (char **) malloc((mod_argc+1) * sizeof(char *));
   mod_argv[0] = const_cast<char *>("spindle");
   for (i = 0; i < sargc && sargv && sargv[i] != NULL; i++) {
      mod_argv[i+1] = sargv[i];
   }
   mod_argv[i+1] = const_cast<char*>("launcher");
   mod_argv[i+2] = NULL;
   mod_argc = i+2;

   string errmsg;
   bool result = gatherAllConfigInfo(mod_argc, mod_argv, false, config, errmsg);
   free(mod_argv);
   if (!result) {
      err_printf("Failed to gather config info for launcher: %s\n", errmsg.c_str());
      fillargs_errmsg = errmsg;
      if (errstr)
         *errstr = const_cast<char*>(fillargs_errmsg.c_str());
      return -1;
   }

   result = config.toSpindleArgs(*params, true);
   if (!result) {
      err_printf("Internal error converting config to args\n");
      fillargs_errmsg = "Internal Error";
      if (errstr)
         *errstr = const_cast<char*>(fillargs_errmsg.c_str());
      return -1;
   }

   params->use_launcher = external_launcher;
   params->startup_type = startup_external;

   if (!(options & SPINDLE_FILLARGS_NOUNIQUEID)) {
      result = config.getUniqueID(params->unique_id, errmsg);
      if (!result) {
         fillargs_errmsg = errmsg;
         if (errstr)
            *errstr = const_cast<char*>(fillargs_errmsg.c_str());
         return -1;
      }
   }
   
   if (!(options & SPINDLE_FILLARGS_NONUMBER)) {
      params->number = config.getNumber();
   }

   return 0;
}

static void *md_data_ptr;

static void printFlag(opt_t &opts, opt_t flag, const char *str, stringstream &ss)
{
   if (!(opts & flag))
       return;
   opts &= ~flag;
   if (ss.tellp())
      ss << ", ";
   ss << str;
}
   
static void printSpindleFlags(opt_t opts) {
   if (!spindle_debug_prints)
      return;
   stringstream ss;
   printFlag(opts, OPT_COBO, "OPT_COBO", ss);
   printFlag(opts, OPT_DEBUG, "OPT_DEBUG", ss);
   printFlag(opts, OPT_FOLLOWFORK, "OPT_FOLLOWFORK", ss);
   printFlag(opts, OPT_PRELOAD, "OPT_PRELOAD", ss);
   printFlag(opts, OPT_PUSH, "OPT_PUSH", ss);
   printFlag(opts, OPT_PULL, "OPT_PULL", ss);
   printFlag(opts, OPT_RELOCAOUT, "OPT_RELOCAOUT", ss);
   printFlag(opts, OPT_RELOCSO, "OPT_RELOCSO", ss);
   printFlag(opts, OPT_RELOCEXEC, "OPT_RELOCEXEC", ss);
   printFlag(opts, OPT_RELOCPY, "OPT_RELOCPY", ss);
   printFlag(opts, OPT_STRIP, "OPT_STRIP", ss);
   printFlag(opts, OPT_NOCLEAN, "OPT_NOCLEAN", ss);
   printFlag(opts, OPT_NOHIDE, "OPT_NOHIDE", ss);
   printFlag(opts, OPT_REMAPEXEC, "OPT_REMAPEXEC", ss);
   printFlag(opts, OPT_LOGUSAGE, "OPT_LOGUSAGE", ss);
   printFlag(opts, OPT_SHMCACHE, "OPT_SHMCACHE", ss);
   printFlag(opts, OPT_SUBAUDIT, "OPT_SUBAUDIT", ss);
   printFlag(opts, OPT_PERSIST, "OPT_PERSIST", ss);
   printFlag(opts, OPT_SEC, "OPT_SEC", ss);
   printFlag(opts, OPT_SESSION, "OPT_SESSION", ss);
   printFlag(opts, OPT_MSGBUNDLE, "OPT_MSGBUNDLE", ss);
   printFlag(opts, OPT_SELFLAUNCH, "OPT_SELFLAUNCH", ss);
   printFlag(opts, OPT_BEEXIT, "OPT_BEEXIT", ss);
   printFlag(opts, OPT_PROCCLEAN, "OPT_PROCCLEAN", ss);
   printFlag(opts, OPT_RSHLAUNCH, "OPT_RSHLAUNCH", ss);
   printFlag(opts, OPT_STOPRELOC, "OPT_STOPRELOC", ss);
   printFlag(opts, OPT_NUMA, "OPT_NUMA", ss);
   printFlag(opts, OPT_OFF, "OPT_OFF", ss);
   printFlag(opts, OPT_PATCHLDSO, "OPT_PATCHLDSO", ss);
   printFlag(opts, OPT_CRASH_HANDLER, "OPT_CRASH_HANDLER", ss);
   printFlag(opts, OPT_CRASH_LOG, "OPT_CRASH_LOG", ss);
   printFlag(opts, OPT_CRASH_ALTSTACK, "OPT_CRASH_ALTSTACK", ss);
   ss << ", ";
   if (OPT_GET_SEC(opts) == OPT_SEC_MUNGE) ss << "OPT_SEC_MUNGE";
   if (OPT_GET_SEC(opts) == OPT_SEC_KEYLMON) ss << "OPT_SEC_KEYLMON";
   if (OPT_GET_SEC(opts) == OPT_SEC_KEYFILE) ss << "OPT_SEC_KEYFILE";
   if (OPT_GET_SEC(opts) == OPT_SEC_NULL) ss << "OPT_SEC_NULL";
   opts &= ~((opt_t) OPT_SEC);
   if (opts) {
      ss << "[Unrecognize opts string " << opts << "]";
   }
   debug_printf("Spindle Options: %s\n", ss.str().c_str());
}

int spindleInitFE(const char **hosts, spindle_args_t *params)
{
   if (params->opts & OPT_OFF)
      return 0;
   
   LOGGING_INIT(const_cast<char *>("FE"));
   debug_printf("Called spindleInitFE\n");

   if (params->opts & OPT_LOGUSAGE)
      logUser();

   setupSecurity(params);

   /* Create preload message before initializing network to detect errors */
   ldcs_message_t *preload_msg = NULL;
   if (params->opts & OPT_PRELOAD) {
      string preload_file = string(params->preloadfile);
      preload_msg = parsePreloadFile(preload_file);
      if (!preload_msg) {
         fprintf(stderr, "Failed to parse preload file %s\n", preload_file.c_str());
         return -1;
      }
   }

   /* Compute hosts size */
   unsigned int hosts_size = 0;
   for (const char **h = hosts; *h != NULL; h++, hosts_size++);

   /* Start RSH launchers */
   if (params->opts & OPT_RSHLAUNCH) {
      init_rsh_launch_fe(params);
   }
   
   /* Start FE server */
   debug_printf("spindle_args_t { number = %lu; port = %u; num_ports = %u; opts = %lu; unique_id = %lu; "
                "use_launcher = %u; startup_type = %u; shm_cache_size = %u; commpaths = %s; "
                "cachepaths = %s; "
                "pythonprefix = %s; preloadfile = %s; bundle_timeout_ms = %u; bundle_cachesize_kb = %u; "
                "crash_log = %s }\n",
                (unsigned long) params->number, params->port, params->num_ports, params->opts, params->unique_id,
                params->use_launcher, params->startup_type, params->shm_cache_size, params->commpaths,
                params->candidate_cachepaths,
                params->pythonprefix, params->preloadfile, params->bundle_timeout_ms,
                params->bundle_cachesize_kb, params->crash_log);
   printSpindleFlags(params->opts);
   debug_printf("Starting FE servers with hostlist of size %u on port %u\n", hosts_size, params->port);
   if (ldcs_audit_server_fe_md_open(const_cast<char **>(hosts), hosts_size,
                                    params->port, params->num_ports, params->unique_id,
                                    &md_data_ptr) == -1) {
      err_printf("Could not start the Spindle server tree\n");
      goto fail;
   }

   /* Broadcast parameters */
   debug_printf("Sending parameters to servers\n");
   void *param_buffer;
   unsigned int param_buffer_size;
   pack_data(params, param_buffer, param_buffer_size);
   ldcs_message_t msg;
   msg.header.type = LDCS_MSG_SETTINGS;
   msg.header.len = param_buffer_size;
   msg.data = static_cast<char *>(param_buffer);
   int result;
   result = ldcs_audit_server_fe_broadcast(&msg, md_data_ptr);
   free(param_buffer);
   if (result == -1) {
      err_printf("Could not send settings to Spindle servers\n");
      goto fail;
   }

   /* Wait for servers to indicate startup */
   if (!sendAndWaitForAlive()) {
      err_printf("Spindle servers did not report ready\n");
      goto fail;
   }

   /* Broadcast preload contents */
   if (preload_msg) {
      debug_printf("Sending message with preload information\n");
      result = ldcs_audit_server_fe_broadcast(preload_msg, md_data_ptr);
      cleanPreloadMsg(preload_msg);
      if (result == -1) {
         err_printf("Could not send preload list to Spindle servers\n");
         return -1;
      }

      /* The root server accepts no client connections while it reads the
       * preload files, so the job must not start until it is done. */
      if (ldcs_audit_server_fe_md_waitfor_preload() == -1) {
         err_printf("Spindle servers did not finish the preload\n");
         return -1;
      }
   }

   return 0;

 fail:
   if (preload_msg)
      cleanPreloadMsg(preload_msg);
   return -1;
}

int spindleWaitForCloseFE(spindle_args_t *params)
{
   if (params->opts & OPT_OFF)
      return 0;
   
   LOGGING_INIT(const_cast<char *>("FE"));   
   if (params->opts & OPT_PERSIST) {
      debug_printf("Warning: blocking for close on a spindle network marked as persistant.  This may permanently hang\n");
   }
   return ldcs_audit_server_fe_md_waitfor_close();
}

int spindleCloseFE(spindle_args_t *params)
{
   pid_t rshpid;
   if (params->opts & OPT_OFF)
      return 0;
   
   LOGGING_INIT(const_cast<char *>("FE"));

   debug_printf("Called spindleCloseFE\n");
   
   ldcs_audit_server_fe_md_close(md_data_ptr);

   if (OPT_GET_SEC(params->opts) == OPT_SEC_KEYFILE) {
      clean_keyfile(params->unique_id);
   }

   if (params->opts & OPT_RSHLAUNCH) {
      rshpid = get_fe_rsh_pid();
      if (rshpid != (pid_t) -1) {
         collect_rsh_pid_fe();
         clear_fe_rsh_pid();
      }
   }
   debug_printf("Finishing call to spindleCloseFE\n");   
   if (params->startup_type == startup_external)
      LOGGING_FINI;

   return 0;
}

pid_t getRSHPidFE()
{   
   return get_fe_rsh_pid();
}

void markRSHPidReapedFE()
{
   clear_fe_rsh_pid();
}

static bool sendAndWaitForAlive()
{
   int result;
   ldcs_message_t alive_req_msg;
   alive_req_msg.header.type = LDCS_MSG_ALIVE_REQ;
   alive_req_msg.header.len = 0;
   alive_req_msg.data = NULL;
   
   result = ldcs_audit_server_fe_broadcast(&alive_req_msg, NULL);
   if (result == -1) {
      debug_printf("Failure sending alive message\n");
      return false;
   }

   result = ldcs_audit_server_fe_md_waitfor_alive();
   if (result == -1) {
      debug_printf("Failure waiting for alive message\n");
      return false;
   }
   return true;
}
