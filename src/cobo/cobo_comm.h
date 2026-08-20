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

#ifndef _COBO_COMM_H
#define _COBO_COMM_H

#define COBO_SUCCESS (0)

#include "ldcs_api.h"

int ldcs_cobo_read_fd(int fd, void* buf, int size);
int ldcs_cobo_write_fd(int fd, void* buf, int size);
int ll_write(int fd, void *buf, size_t count);
int ll_send_nosignal(int fd, void *buf, size_t count);
int ll_read(int fd, void *buf, size_t count);
int write_msg(int fd, ldcs_message_t *msg);
int write_msg_nosignal(int fd, ldcs_message_t *msg);

#endif /* _COBO_COMM_H */
