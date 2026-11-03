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

#include "ldcs_api.h"
#include "ldcs_api_listen.h"
#include "ldcs_audit_server_process.h"
#include "ldcs_audit_server_handlers.h"

int _ldcs_server_CB ( int infd, int serverid, void *data ) {
   (void)infd;
   int rc=0;
   ldcs_process_data_t *ldcs_process_data = (ldcs_process_data_t *) data ;
   int nc, fd, more_avail, connid;
   double cb_starttime;

   cb_starttime=ldcs_get_time();

   more_avail=1;
   while(more_avail) {
    
      /* add new client */
      if (ldcs_process_data->client_table_used >= ldcs_process_data->client_table_size) {
         ldcs_process_data->client_table_size = ldcs_process_data->client_table_used + 16;
         ldcs_process_data->client_table = realloc(ldcs_process_data->client_table, 
                                                   ldcs_process_data->client_table_size * sizeof(ldcs_client_t));
         for(nc=ldcs_process_data->client_table_used;(nc<ldcs_process_data->client_table_used + 16);nc++) {
            ldcs_process_data->client_table[nc].state=LDCS_CLIENT_STATUS_FREE;
         }
      }
      for(nc=0;(nc<ldcs_process_data->client_table_size);nc++) {
         if (ldcs_process_data->client_table[nc].state==LDCS_CLIENT_STATUS_FREE) break;
      }
      if(nc==ldcs_process_data->client_table_size) _error("internal error with client table (table full)");
    
      connid = ldcs_open_server_connections(serverid, nc, &more_avail);
      if (connid == -1) {
         debug_printf("No new client connection accepted\n");
         continue;
      }
      ldcs_process_data->client_table[nc].connid       = connid;
      ldcs_process_data->client_table[nc].state        = LDCS_CLIENT_STATUS_ACTIVE;
      ldcs_process_data->client_table[nc].null_msg_cnt = 0;    
      ldcs_process_data->client_table[nc].query_open   = 0;
      ldcs_process_data->client_table[nc].existance_query = 0;
      ldcs_process_data->client_table[nc].is_stat      = 0;
      ldcs_process_data->client_table[nc].is_loader    = 0;
      ldcs_process_data->client_table[nc].is_lstat     = 0;
      ldcs_process_data->client_table[nc].is_dso       = 0;
      ldcs_process_data->client_table[nc].numa_node    = 0;      
      ldcs_process_data->client_table[nc].lrank        = ldcs_process_data->client_counter;
      ldcs_process_data->client_table[nc].query_localpath = NULL;
      ldcs_process_data->client_table[nc].query_is_numa_replicated = 0;
      ldcs_process_data->client_table[nc].tls_info = NULL;
      ldcs_process_data->client_table[nc].crash_exe = NULL;
      ldcs_process_data->client_table[nc].crash_corepath = NULL;
      ldcs_process_data->client_table_used++;
      ldcs_process_data->client_counter++;
      ldcs_process_data->clients_live++;
      debug_printf("Adding new client %d\n", nc);
    
      /* register client fd to listener */
      fd=ldcs_get_fd(ldcs_process_data->client_table[nc].connid);
      if (fd != -1)
         ldcs_listen_register_fd(fd, nc, &_ldcs_client_CB, (void *) ldcs_process_data);
      ldcs_process_data->server_stat.num_connections++;

      handle_client_start(ldcs_process_data, nc);
   }

   fd = ldcs_get_aux_fd();
   if (fd != -1)
      ldcs_listen_register_fd(fd, CLIENT_CB_AUX_FD, &_ldcs_client_CB, (void *) ldcs_process_data);

   /* remember timestamp of connect of first client */
   if(ldcs_process_data->server_stat.starttime<0) {
      ldcs_process_data->server_stat.starttime=cb_starttime;
   }
   ldcs_process_data->server_stat.server_cb.cnt++;
   ldcs_process_data->server_stat.server_cb.time+=(ldcs_get_time()-cb_starttime);

   return(rc);
}



