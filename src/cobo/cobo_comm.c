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

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <stdarg.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <poll.h>
#include <assert.h>
#include "ldcs_cobo.h"
#include "spindle_debug.h"
#include "cobo_comm.h"
#include "config.h"

/* read size bytes into buf from fd, retry if necessary */
static int ldcs_cobo_read_fd_w_timeout(int fd, void* buf, int size, int usecs)
{
    int rc;
    int n = 0;
    char* offset = (char*) buf;

    struct pollfd fds;
    fds.fd      = fd;
    fds.events  = POLLIN;
    fds.revents = 0x0;

    while (n < size) {
        /* poll the connection with a timeout value */
        int poll_rc = poll(&fds, 1, usecs);
        if (poll_rc < 0) {
            err_printf("Polling file descriptor for read (read(fd=%d,offset=%p,size=%d) %m errno=%d)\n",
                       fd, offset, size-n, errno);
            return -1;
        } else if (poll_rc == 0) {
            return -1;
        }

        /* check the revents field for errors */
        if (fds.revents & POLLHUP) {
            debug_printf3("Hang up error on poll for read(fd=%d,offset=%p,size=%d)\n",
                          fd, offset, size-n);
            return -1;
        }

        if (fds.revents & POLLERR) {
            debug_printf3("Error on poll for read(fd=%d,offset=%p,size=%d)\n",
                          fd, offset, size-n);
            return -1;
        }

        if (fds.revents & POLLNVAL) {
            err_printf("Invalid request on poll for read(fd=%d,offset=%p,size=%d)\n",
                       fd, offset, size-n);
            return -1;
        }

        if (!(fds.revents & POLLIN)) {
            err_printf("No errors found, but POLLIN is not set for read(fd=%d,offset=%p,size=%d)\n",
                       fd, offset, size-n);
            return -1;
        }

        /* poll returned that fd is ready for reading */
	rc = read(fd, offset, size - n);

	if (rc < 0) {
	    if(errno == EINTR || errno == EAGAIN) { continue; }
            err_printf("Reading from file descriptor (read(fd=%d,offset=%p,size=%d) %m errno=%d)\n",
                       fd, offset, size-n, errno);
	    return rc;
	} else if(rc == 0) {
            err_printf("Unexpected return code of 0 from read from file descriptor (read(fd=%d,offset=%p,size=%d) revents=%x)\n",
                       fd, offset, size-n, fds.revents);
	    return -1;
	}

	offset += rc;
	n += rc;
    }

    return n;
}

/* read size bytes into buf from fd, retry if necessary */
int ldcs_cobo_read_fd(int fd, void* buf, int size)
{
    return ldcs_cobo_read_fd_w_timeout(fd, buf, size, -1);
}

/* write size bytes from buf into fd, retry if necessary */
static int ldcs_cobo_write_fd_w_suppress(int fd, void* buf, int size, int suppress)
{
    int rc;
    int n = 0;
    char* offset = (char*) buf;

    while (n < size) {
	rc = write(fd, offset, size - n);

	if (rc < 0) {
	    if(errno == EINTR || errno == EAGAIN) { continue; }
            if (!suppress) {
                err_printf("Writing to file descriptor (write(fd=%d,offset=%p,size=%d) %m errno=%d)\n",
                           fd, offset, size-n, errno);
            } else {
                debug_printf3("Writing to file descriptor (write(fd=%d,offset=%p,size=%d) %m errno=%d)\n",
                           fd, offset, size-n, errno);
            }
	    return rc;
	} else if(rc == 0) {
            if (!suppress) {
                err_printf("Unexpected return code of 0 from write to file descriptor (write(fd=%d,offset=%p,size=%d))\n",
                           fd, offset, size-n);
            } else {
                debug_printf3("Unexpected return code of 0 from write to file descriptor (write(fd=%d,offset=%p,size=%d))\n",
                           fd, offset, size-n);
            }
	    return -1;
	}

	offset += rc;
	n += rc;
    }

    return n;
}

/* write size bytes from buf into fd, retry if necessary */
int ldcs_cobo_write_fd(int fd, void* buf, int size)
{
    return ldcs_cobo_write_fd_w_suppress(fd, buf, size, 0);
}

int ll_read(int fd, void *buf, size_t count)
{
   int result, error;
   size_t pos = 0;
   unsigned char *cbuf = (unsigned char *) buf;

   while (pos < count) {
      errno = 0;
      result = read(fd, cbuf + pos, count - pos);
      debug_printf3("Read %d bytes from network: 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x...\n", result,
                    result > 0 ? ((int) cbuf[pos+0]) : 0,
                    result > 1 ? ((int) cbuf[pos+1]) : 0,
                    result > 2 ? ((int) cbuf[pos+2]) : 0,
                    result > 3 ? ((int) cbuf[pos+3]) : 0,
                    result > 4 ? ((int) cbuf[pos+4]) : 0,
                    result > 5 ? ((int) cbuf[pos+5]) : 0,
                    result > 6 ? ((int) cbuf[pos+6]) : 0,
                    result > 7 ? ((int) cbuf[pos+7]) : 0);
      if (result == -1 || result == 0) {
         error = errno;
         if (error == EINTR || error == EAGAIN)
            continue;
         if (error == 0) {
            debug_printf3("ll_read terminated without a return result\n");
            return -1;
         }
         err_printf("Error reading from cobo FD %d with return result %d: %s\n", fd, result, strerror(error));
         return -1;
      }
      pos += result;
   }
   return 0;
}

int ll_write(int fd, void *buf, size_t count)
{
   int result, error;
   size_t pos = 0;
   unsigned char *cbuf = (unsigned char *) buf;

   debug_printf3("Have %lu bytes at %p to write to network\n", count, buf);
   
   while (pos < count) {
      result = write(fd, cbuf + pos, count - pos);
      debug_printf3("Wrote %d bytes at %p to network: 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x...\n", result, cbuf + pos,
                    result > 0 ? ((int) cbuf[pos+0]) : 0,
                    result > 1 ? ((int) cbuf[pos+1]) : 0,
                    result > 2 ? ((int) cbuf[pos+2]) : 0,
                    result > 3 ? ((int) cbuf[pos+3]) : 0,
                    result > 4 ? ((int) cbuf[pos+4]) : 0,
                    result > 5 ? ((int) cbuf[pos+5]) : 0,
                    result > 6 ? ((int) cbuf[pos+6]) : 0,
                    result > 7 ? ((int) cbuf[pos+7]) : 0) ;                         

      if (result == -1) {
         if (errno == EINTR || errno == EAGAIN)
            continue;
         error = errno;
         err_printf("Error writing to cobo FD %d: %s\n", fd, strerror(error));
         return -1;
      }
      else if (result == 0) {
         err_printf("Unexpected exit from peer\n");
         return -1;
      }
      pos += result;
   }
   return 0;
}

int write_msg(int fd, ldcs_message_t *msg)
{
   int result = ll_write(fd, msg, sizeof(*msg));
   if (result == -1) {
      return -1;
   }

   if (msg->header.len && msg->data) {
      result = ll_write(fd, msg->data, msg->header.len);
      if (result == -1) {
         return -1;
      }
   }

   return 0;
}

/* ll_send_nosignal is a variation on ll_write which uses
 * send(..., MSG_NOSIGNAL) to avoid SIGPIPE when writing to
 * a broken socket.
 */
int ll_send_nosignal(int fd, void *buf, size_t count)
{
   int error;
   ssize_t result;
   size_t pos = 0;
   unsigned char *cbuf = (unsigned char *) buf;

   debug_printf3("Have %lu bytes at %p to write to network\n", count, buf);

   while (pos < count) {
      result = send(fd, cbuf + pos, count - pos, MSG_NOSIGNAL);
      debug_printf3("Wrote %d bytes at %p to network: 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x...\n", (int) result, cbuf + pos,
                    result > 0 ? ((int) cbuf[pos+0]) : 0,
                    result > 1 ? ((int) cbuf[pos+1]) : 0,
                    result > 2 ? ((int) cbuf[pos+2]) : 0,
                    result > 3 ? ((int) cbuf[pos+3]) : 0,
                    result > 4 ? ((int) cbuf[pos+4]) : 0,
                    result > 5 ? ((int) cbuf[pos+5]) : 0,
                    result > 6 ? ((int) cbuf[pos+6]) : 0,
                    result > 7 ? ((int) cbuf[pos+7]) : 0);
      if (result == -1 && (errno == EINTR || errno == EAGAIN))
         continue;
      if (result <= 0) {
         error = errno;
         err_printf("Error writing to cobo FD %d: %s\n", fd, strerror(error));
         return -1;
      }
      pos += result;
   }
   debug_printf3("Sent %lu bytes to fd %d\n", count, fd);
   return 0;
}

/* write_msg_nosignal is a variation on write_msg
 * which uses ll_send_nosignal instead of ll_send
 * to avoid SIGPIPE when writing to a broken socket.
 */
int write_msg_nosignal(int fd, ldcs_message_t *msg)
{
   int result = ll_send_nosignal(fd, msg, sizeof(*msg));
   if (result == -1) {
      return -1;
   }

   if (msg->header.len && msg->data) {
      result = ll_send_nosignal(fd, msg->data, msg->header.len);
      if (result == -1) {
         return -1;
      }
   }

   return 0;
}
