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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/inotify.h>
#include <errno.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "ldcs_api.h"
#include "ldcs_api_listen.h"
#include "ldcs_audit_server_process.h"
#include "ldcs_audit_server_filemngt.h"
#include "ldcs_audit_server_md.h"
#include "ldcs_audit_server_handlers.h"
#include "ldcs_audit_server_crash_handler.h"
#include "ldcs_cache.h"
#include "spindle_launch.h"
#include "ldcs_audit_server_requestors.h"
#include "msgbundle.h"
#include "exitnote.h"
#include "cleanup_proc.h"
#include "parseloc.h"

//#define GPERFTOOLS
#if defined(GPERFTOOLS)
#include <gperftools/profiler.h>
#endif

ldcs_process_data_t ldcs_process_data;
unsigned int opts;

int _listen_exit_loop_cb_func ( int num_fds,  void * data) {
  (void)num_fds;
  int rc=0;
  ldcs_process_data_t *ldcs_process_data = ( ldcs_process_data_t *) data ;

  debug_printf3("check exit condition open_conn=%d clients_connected=%d \n",
	       ldcs_process_data->client_table_used,
	       ldcs_process_data->clients_connected);
  if(ldcs_process_data->client_table_used>1) {
    ldcs_process_data->clients_connected=1;
  } else {
    if(ldcs_process_data->clients_connected == 1) {

      /* session has ended, unregister listener for new client connections */
      ldcs_listen_unregister_fd(ldcs_process_data->serverfd);

      /* unregister also md support (multi-daemon) */
      ldcs_audit_server_md_unregister_fd(ldcs_process_data);

    }
  }
  return(rc);
}

int ldcs_audit_server_network_setup(unsigned int port, unsigned int num_ports, unique_id_t unique_id, 
                                    void **packed_setup_data, int *data_size)
{
   int result;
   debug_printf2("Setting up server data structure\n");

   memset(&ldcs_process_data, 0, sizeof(ldcs_process_data));

   /* Initialize server->server network */
   if (ldcs_audit_server_md_init(port, num_ports, unique_id, &ldcs_process_data) == -1) {
      err_printf("Error initializing server network\n");
      return -1;
   }

   /* Use network to broadcast configuration parameters */
   ldcs_message_t msg;
   msg.header.type = 0;
   msg.header.len = 0;
   msg.data = NULL;
   debug_printf2("Reading setup message from parent\n");
   result = ldcs_audit_server_md_recv_from_parent(&msg);
   if (result == -1) {
      err_printf("Error reading setup message from parent\n");
      return -1;
   }
   assert(msg.header.type == LDCS_MSG_SETTINGS);
   result = ldcs_audit_server_md_broadcast(&ldcs_process_data, &msg);
   if (result == -1) {
      err_printf("Error broadcast setup message to children\n");
      return -1;
   }

   *packed_setup_data = msg.data;
   *data_size = msg.header.len;

   return 0;
}

#if defined(GPERFTOOLS)
void startprofile(spindle_args_t *args)
{
   char filename[4096];
   char hostname[257];
   char *home = getenv("HOME");
   if (!home || !*home)
      home = ldcs_process_data.commpath;
   gethostname(hostname, sizeof(hostname));
   snprintf(filename, 4096, "%s/spindled.%lu.%s.%d.prof", home, (unsigned long) args->number, hostname, getpid());
   ProfilerStart(filename);
}

void stopprofile()
{
   ProfilerFlush();
   ProfilerStop();
}
#else
void startprofile(spindle_args_t *args)
{
   (void) args;
}

void stopprofile()
{
}
#endif

int ldcs_audit_server_process(spindle_args_t *args)
{
   int serverid, fd, result;

   startprofile(args);

   debug_printf3("Initializing server data structures\n");
   ldcs_process_data.commpath = args->commpath;
   ldcs_process_data.cachepaths = args->candidate_cachepaths;
   ldcs_process_data.cachepath = NULL;
   ldcs_process_data.cachepath_bitidx = 0;
   ldcs_process_data.number = args->number;
   ldcs_process_data.pythonprefix = args->pythonprefix;
   ldcs_process_data.crash_log = args->crash_log;
   ldcs_process_data.localprefix = args->local_prefixes;
   ldcs_process_data.exec_excludes = args->exec_excludes;
   ldcs_process_data.numa_substrs = args->numa_files;
   ldcs_process_data.numa_excludes = args->numa_excludes;
   ldcs_process_data.md_port = args->port;
   ldcs_process_data.opts = args->opts;
   ldcs_process_data.msgbundle_cache_size_kb = args->bundle_cachesize_kb;
   ldcs_process_data.msgbundle_timeout_ms = args->bundle_timeout_ms;
   ldcs_process_data.dso_requests = new_requestor_list();
   ldcs_process_data.file_requests = new_requestor_list();   
   ldcs_process_data.completed_dso_requests = new_requestor_list();
   ldcs_process_data.completed_file_requests = new_requestor_list();   
   ldcs_process_data.pending_stat_requests = new_requestor_list();
   ldcs_process_data.completed_stat_requests = new_requestor_list();
   ldcs_process_data.pending_lstat_requests = new_requestor_list();
   ldcs_process_data.completed_lstat_requests = new_requestor_list();
   ldcs_process_data.pending_ldso_requests = new_requestor_list();
   ldcs_process_data.completed_ldso_requests = new_requestor_list();
   ldcs_process_data.pending_tls_requests = new_requestor_list();
   ldcs_process_data.completed_tls_requests = new_requestor_list();
   ldcs_process_data.handling_bundle = 0;
   ldcs_process_data.exit_note_done = 0;
   ldcs_process_data.exit_on_client_close = 0;
   ldcs_process_data.num_exited_children_peers = 0;
   ldcs_process_data.num_exited_parents = 0;
   ldcs_process_data.num_alives = 0;
   ldcs_process_data.crash_sites = NULL;
   ldcs_process_data.crash_sites_count = 0;
   ldcs_process_data.crash_sites_cap = 0;

   if (ldcs_process_data.opts & OPT_PULL) {
      debug_printf("Using PULL model\n");
      ldcs_process_data.dist_model = LDCS_PULL;
   }
   else if (ldcs_process_data.opts & OPT_PUSH) {
      debug_printf("Using PUSH model\n");
      ldcs_process_data.dist_model = LDCS_PUSH;
   }
   else {
      err_printf("Neither push nor pull options were set\n");
      assert(0);
   }

   _ldcs_server_stat_init(&ldcs_process_data.server_stat);

   {
      char buffer[65];
      gethostname(buffer, 65);
      ldcs_process_data.hostname = strdup(buffer);
   }
   ldcs_process_data.server_stat.hostname=ldcs_process_data.hostname;

   if (ldcs_process_data.opts & OPT_PROCCLEAN)
      init_cleanup_proc(ldcs_process_data.cachepath, ldcs_process_data.commpath);

   /* Calculate location of cache */
   debug_printf2("Calculating cache path location\n");
   determineValidCachePaths(
           &ldcs_process_data.cachepath_bitidx,
           ldcs_process_data.cachepaths,
           ldcs_process_data.number );
   result = handle_cachepath_consensus(&ldcs_process_data);
   if (result == -1) {
      err_printf("Could not determine cachepath consensus\n");
      return -1;
   }   

   /* Setup connections for clients to start connecting */
   debug_printf2("Initializing connections for clients at %s and %lu\n",
                 ldcs_process_data.commpath, (unsigned long) ldcs_process_data.number);
   serverid = ldcs_create_server(ldcs_process_data.commpath, ldcs_process_data.number);
   if (serverid == -1) {
      err_printf("Unable to setup area for client connections\n");
      return -1;
   }
   ldcs_process_data.serverid = serverid;
   ldcs_audit_server_md_test_fault_point("ready", ldcs_process_data.md_rank);
   fd = ldcs_get_fd(serverid);
   ldcs_process_data.serverfd = fd;
  
   ldcs_audit_server_md_register_fd(&ldcs_process_data);
  
   /* register server listen fd to listener */
   if (fd != -1)
      ldcs_listen_register_fd(fd, serverid, &_ldcs_server_CB, (void *) &ldcs_process_data);

   if (args->opts & OPT_BEEXIT) {
      fd = createExitNote(args->commpath);
      if (fd != -1) {
         ldcs_listen_register_fd(fd, serverid, exit_note_cb, (void *) &ldcs_process_data);
      }
   }
  
   debug_printf3("Initializing cache\n");
   ldcs_cache_init();

   msgbundle_init(&ldcs_process_data);

   fd = getForceExitFd();
   if (fd != -1) {
      ldcs_listen_register_fd(fd, serverid, forceExitCB, (void *) &ldcs_process_data);
   }
   
   return 0;
}

int ldcs_audit_server_run()
{
   /* start loop */
   debug_printf2("Entering server loop\n");
   ldcs_listen();

   crash_log_root_write(&ldcs_process_data);
  
   ldcs_process_data.server_stat.listen_time= ldcs_get_time() - ldcs_process_data.server_stat.starttime;
   ldcs_process_data.server_stat.select_time=
      ldcs_process_data.server_stat.listen_time
      - ldcs_process_data.server_stat.client_cb.time
      - ldcs_process_data.server_stat.server_cb.time
      - ldcs_process_data.server_stat.md_cb.time;


   _ldcs_server_stat_print(&ldcs_process_data.server_stat);
  
   debug_printf("destroy server (%s,%lu)\n", ldcs_process_data.commpath, (unsigned long) ldcs_process_data.number);
   ldcs_destroy_server(ldcs_process_data.serverid);
  
   /* destroy md support (multi-daemon) */
   ldcs_audit_server_md_destroy(&ldcs_process_data);

   msgbundle_done(&ldcs_process_data);

   crash_free_tables(&ldcs_process_data);

   /* destroy file cache */
   if (!(ldcs_process_data.opts & OPT_NOCLEAN)) {
      ldcs_audit_server_filemngt_clean();
   }

   /* Clean shm segment */
   if (ldcs_process_data.opts & OPT_SHMCACHE) {
      char shm_name[128];
      snprintf(shm_name, sizeof(shm_name), "biter_shm.%lu", (unsigned long) ldcs_process_data.number);
      shm_unlink(shm_name);
   }
  
   return 0;
}

 /* Statistic functions */
 void _ldcs_server_stat_init_entry ( ldcs_server_stat_entry_t *entry ) {
   entry->cnt=0.0;
   entry->bytes=0.0;
   entry->time=0.0;
 }

 int _ldcs_server_stat_init ( ldcs_server_stat_t *server_stat ) {
   int rc=0;
   server_stat->md_rank=0;
   server_stat->md_size=0;
   server_stat->md_fan_out=0;
   server_stat->num_connections=0;
   server_stat->starttime=-1;

   _ldcs_server_stat_init_entry(&server_stat->libread);   
   _ldcs_server_stat_init_entry(&server_stat->libstore);
   _ldcs_server_stat_init_entry(&server_stat->metadata);   
   _ldcs_server_stat_init_entry(&server_stat->libdist);
   _ldcs_server_stat_init_entry(&server_stat->procdir);
   _ldcs_server_stat_init_entry(&server_stat->distdir);
   _ldcs_server_stat_init_entry(&server_stat->client_cb);
   _ldcs_server_stat_init_entry(&server_stat->server_cb);
   _ldcs_server_stat_init_entry(&server_stat->md_cb);
   _ldcs_server_stat_init_entry(&server_stat->clientmsg);
   _ldcs_server_stat_init_entry(&server_stat->bcast);
   _ldcs_server_stat_init_entry(&server_stat->preload);

   return(rc);
 }

 /* Statistic functions */
 int _ldcs_server_stat_print ( ldcs_server_stat_t *server_stat ) {
   int rc=0;
   debug_printf("SERVER[%02d] STAT: #conn=%2d md_size=%2d md_fan_out=%2d listen_time=%8.4f select_time=%8.4f ts_first_connect=%16.6f hostname=%s\n",
	   server_stat->md_rank, 
	   server_stat->num_connections,	
	   server_stat->md_size,
	   server_stat->md_fan_out,
	   server_stat->listen_time,
	   server_stat->select_time,
	   server_stat->starttime,
	   server_stat->hostname );

#define MYFORMAT "SERVER[%02d] STAT:  %-10s, #cnt=%5d, bytes=%8.2f MB, time=%8.4f sec\n"

  debug_printf(MYFORMAT,
	  server_stat->md_rank,"libread",
	  server_stat->libread.cnt,
	  server_stat->libread.bytes/1024.0/1024.0,
	  server_stat->libread.time );

  debug_printf(MYFORMAT,
	  server_stat->md_rank,"libstore",
	  server_stat->libstore.cnt,
	  server_stat->libstore.bytes/1024.0/1024.0,
	  server_stat->libstore.time );

  debug_printf(MYFORMAT,
	  server_stat->md_rank,"metadata",
	  server_stat->metadata.cnt,
	  server_stat->metadata.bytes/1024.0/1024.0,
	  server_stat->metadata.time );

  debug_printf(MYFORMAT,
	  server_stat->md_rank,"libdist",
	  server_stat->libdist.cnt,
	  server_stat->libdist.bytes/1024.0/1024.0,
	  server_stat->libdist.time );

  debug_printf(MYFORMAT,
	  server_stat->md_rank,"procdir",
	  server_stat->procdir.cnt,
	  server_stat->procdir.bytes/1024.0/1024.0,
	  server_stat->procdir.time );

  debug_printf(MYFORMAT,
	  server_stat->md_rank,"distdir",
	  server_stat->distdir.cnt,
	  server_stat->distdir.bytes/1024.0/1024.0,
	  server_stat->distdir.time );

  debug_printf(MYFORMAT,
	  server_stat->md_rank,"client_cb",
	  server_stat->client_cb.cnt,
	  server_stat->client_cb.bytes/1024.0/1024.0,
	  server_stat->client_cb.time );

  debug_printf(MYFORMAT,
	  server_stat->md_rank,"server_cb",
	  server_stat->server_cb.cnt,
	  server_stat->server_cb.bytes/1024.0/1024.0,
	  server_stat->server_cb.time );

  debug_printf(MYFORMAT,
	  server_stat->md_rank,"md_cb",
	  server_stat->md_cb.cnt,
	  server_stat->md_cb.bytes/1024.0/1024.0,
	  server_stat->md_cb.time );

  debug_printf(MYFORMAT,
	  server_stat->md_rank,"cl_msg_avg",
	  server_stat->clientmsg.cnt/((server_stat->num_connections>0)?server_stat->num_connections:1),
	  server_stat->clientmsg.bytes/1024.0/1024.0,
	  server_stat->clientmsg.time/((server_stat->num_connections>0)?server_stat->num_connections:1) );

  debug_printf(MYFORMAT,
	  server_stat->md_rank,"bcast",
	  server_stat->bcast.cnt,
	  server_stat->bcast.bytes/1024.0/1024.0,
	  server_stat->bcast.time );

  debug_printf(MYFORMAT,
	  server_stat->md_rank,"preload_cb",
	  server_stat->preload.cnt,
	  server_stat->preload.bytes/1024.0/1024.0,
	  server_stat->preload.time );

  return(rc);
}

requestor_list_t metadata_pending_requests(ldcs_process_data_t *procdata, metadata_t mdtype)
{
   switch (mdtype) {
      case metadata_none: break;
      case metadata_stat: return procdata->pending_stat_requests;
      case metadata_lstat: return procdata->pending_lstat_requests;
      case metadata_loader: return procdata->pending_ldso_requests;
   }
   assert(0);
   return (requestor_list_t) NULL;
}

requestor_list_t metadata_completed_requests(ldcs_process_data_t *procdata, metadata_t mdtype)
{
   switch (mdtype) {
      case metadata_none: break;
      case metadata_stat: return procdata->completed_stat_requests;
      case metadata_lstat: return procdata->completed_lstat_requests;
      case metadata_loader: return procdata->completed_ldso_requests;
   }
   assert(0);
   return (requestor_list_t) NULL;
}
