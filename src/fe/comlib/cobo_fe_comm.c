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

#include "ldcs_cobo.h"
#include "cobo_comm.h"
#include "spindle_debug.h"
#include "ldcs_api.h"
#include "fe_comm.h"
#include "config.h"
#include <assert.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

static int read_msg(int fd, ldcs_message_t *msg)
{
   int result;

   result = ll_read(fd, msg, sizeof(*msg));
   if (result == -1)
      return -1;

   if (msg->header.len) {
      msg->data = (char *) malloc(msg->header.len);
      result = ll_read(fd, msg->data, msg->header.len);
      if (result == -1) {
         free(msg->data);
         return -1;
      }
   }
   else {
      msg->data = NULL;
   }

   return 0;
}

int ldcs_audit_server_fe_md_open ( char **hostlist, int numhosts, unsigned int port, unsigned int num_ports,
                                   unique_id_t unique_id, 
                                   void **data  ) {
   (void)data;
   int rc=0;
   int *portlist;
   int root_fd, ack;
   unsigned int i;

   assert(num_ports >= 1);
   portlist = malloc(sizeof(int) * (num_ports+1));
   for (i = 0; i < num_ports; i++) {
      portlist[i] = port + i;
   }
   portlist[num_ports] = 0;

   debug_printf2("Opening with port %d - %d\n", portlist[0], portlist[num_ports-1]);
   rc = cobo_server_open(unique_id, hostlist, numhosts, portlist, num_ports);
   free(portlist);
   if (rc != COBO_SUCCESS) {
      err_printf("Could not connect to the root Spindle server\n");
      return -1;
   }

   if (cobo_server_get_root_socket(&root_fd) != COBO_SUCCESS) {
      err_printf("Failed to establish connection to the root Spindle server\n");
      return -1;
   }

   if (ldcs_cobo_read_fd_timeout(root_fd, &ack, sizeof(ack), 2 * cobo_get_startup_timeout_ms()) < 0) {
      err_printf("Spindle servers did not acknowledge startup\n");
      return -1;
   }
   if (ack != COBO_READY_ACK) {
      err_printf("Unexpected startup ack message %d from Spindle servers\n", ack);
      return -1;
   }

   return 0;
}

int ldcs_audit_server_fe_md_waitfor_close()
{
   int root_fd, result;
   ldcs_message_t out_msg;

   debug_printf2("Blocking while waiting for spindle exit\n");

   cobo_server_get_root_socket(&root_fd);
   for (;;) {
      memset(&out_msg, 0, sizeof(out_msg));
      result = read_msg(root_fd, &out_msg);
      if (result == -1) {
         err_printf("ERROR reading message while waiting for server close\n");
         return -1;
      }
      if (out_msg.header.type == LDCS_MSG_EXIT_READY)
         return 0;
      err_printf("Unexpected message of type %d\n", (int) out_msg.header.type);
   }
}

int ldcs_audit_server_fe_md_waitfor_alive()
{
   int fd;
   ldcs_message_t out_msg;

   debug_printf2("Blocking while waiting for spindle alive\n");

   cobo_server_get_root_socket(&fd);

   memset(&out_msg, 0, sizeof(out_msg));
   if (read_msg_timeout(fd, &out_msg, cobo_get_startup_timeout_ms()) == -1) {
      err_printf("No alive response from Spindle servers within %d seconds\n", cobo_get_startup_timeout_ms() / 1000);
      return -1;
   }
   if (out_msg.header.type == LDCS_MSG_ALIVE_RESP)
      return 0;
   err_printf("Unexpected message of type %d\n", (int) out_msg.header.type);
   return -1;
}

int ldcs_audit_server_fe_md_waitfor_preload()
{
   int fd;
   ldcs_message_t out_msg;

   debug_printf2("Blocking while waiting for spindle preload\n");

   cobo_server_get_root_socket(&fd);

   memset(&out_msg, 0, sizeof(out_msg));
   if (read_msg(fd, &out_msg) == -1) {
      err_printf("ERROR reading message while waiting for server preload\n");
      return -1;
   }
   if (out_msg.header.type == LDCS_MSG_PRELOAD_DONE)
      return 0;
   err_printf("Unexpected message of type %d\n", (int) out_msg.header.type);
   return -1;
}

int ldcs_audit_server_fe_md_close ( void *data  ) {
   (void)data;
   ldcs_message_t out_msg;
   int root_fd;

   debug_printf("Sending exit message to daemons\n");
   out_msg.header.type = LDCS_MSG_EXIT;
   out_msg.header.len = 0;
   out_msg.data = NULL;

   cobo_server_get_root_socket(&root_fd);
   /* We use write_msg_nosignal to avoid SIGPIPE if the socket is broken.
    * We're exiting here, so ignore if the server already exited. */
   write_msg_nosignal(root_fd, &out_msg);
   return cobo_server_close();
}

int ldcs_audit_server_fe_broadcast(ldcs_message_t *msg, void *data)
{
   (void)data;
   int root_fd;

   debug_printf("Broadcasting message %s to daemons\n",_message_type_to_str( msg->header.type ) );

   cobo_server_get_root_socket(&root_fd);
   return write_msg_nosignal(root_fd, msg);
}


