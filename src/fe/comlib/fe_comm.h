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

#if !defined(FE_COMM_H_)
#define FE_COMM_H_

#if defined(__cplusplus)
extern "C" {
#endif

#include "ldcs_api.h"
#include "spindle_launch.h"

int ldcs_audit_server_fe_md_open(char **hostlist, int numhosts, unsigned int port, unsigned int num_ports,
                                 unique_id_t unique_id, void **data);
int ldcs_audit_server_fe_md_close(void *data);
int ldcs_audit_server_fe_md_waitfor_close();
int ldcs_audit_server_fe_md_waitfor_alive();
int ldcs_audit_server_fe_md_waitfor_preload();
int ldcs_audit_server_fe_broadcast(ldcs_message_t *msg, void *data);

#if defined(__cplusplus)
}
#endif

#endif
