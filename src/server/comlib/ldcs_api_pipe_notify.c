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
#include "ldcs_api_pipe_notify.h"

/* ************************************************************** */
/* Nofify FD list                                                 */
/* ************************************************************** */
#define MAX_FD 10

struct fdlist_nt_entry_t
{
  int   inuse;
  int   ntfd;
  int   wd;
  int   buffer_len;
  int   buffer_next;
  int   buffer_enddata;
  char *buffer;
};

struct fdlist_nt_entry_t ldcs_fdlist_nt[MAX_FD];
int ldcs_fdlist_nt_cnt=-1;

int nt_get_new_fd () {
  int fd;
  if(ldcs_fdlist_nt_cnt==-1) {
    /* init fd list */
    for(fd=0;fd<MAX_FD;fd++) ldcs_fdlist_nt[fd].inuse=0;
  }
  if(ldcs_fdlist_nt_cnt+1<MAX_FD) {

    fd=0;
    while ( (fd<MAX_FD) && (ldcs_fdlist_nt[fd].inuse==1) ) fd++;
    ldcs_fdlist_nt[fd].inuse=1;
    ldcs_fdlist_nt_cnt++;
    return(fd);
  } else {
    return(-1);
  }
}

void nt_free_fd (int fd) {
  ldcs_fdlist_nt[fd].inuse=0;
  ldcs_fdlist_nt_cnt--;
}

/* reasonable guess as to size of 1024 events */
#define EVENT_SIZE  (sizeof (struct inotify_event))
#define BUF_LEN     (1024 * (EVENT_SIZE + 16))


/* ************************************************************** */
/* inotify wrapper                                                */
/* ************************************************************** */

int ldcs_notify_init(char *path) {
  int fd=-1;

  fd=nt_get_new_fd();
  if(fd<0) return(-1);
  
  ldcs_fdlist_nt[fd].inuse= 1;
  ldcs_fdlist_nt[fd].ntfd = inotify_init();
  if (ldcs_fdlist_nt[fd].ntfd < 0) {        
    _error("failure while running inotify_init");
  }
  
  debug_printf3("add watch: ntfd=%d dir=%s\n",ldcs_fdlist_nt[fd].ntfd, path);
  ldcs_fdlist_nt[fd].wd = inotify_add_watch(ldcs_fdlist_nt[fd].ntfd, path, IN_CREATE);
  if (ldcs_fdlist_nt[fd].wd < 0) {
    _error ("inotify_add_watch");  
  }

  ldcs_fdlist_nt[fd].buffer_len=BUF_LEN;
  ldcs_fdlist_nt[fd].buffer=malloc(ldcs_fdlist_nt[fd].buffer_len);
  ldcs_fdlist_nt[fd].buffer_next=-1; 	/* nothing read */

   return(fd);
 }

int ldcs_notify_destroy(int fd) {
  int rc=0;
  if ((fd<0) || (fd>MAX_FD) )  _error("wrong fd");
  
  rc=inotify_rm_watch(ldcs_fdlist_nt[fd].ntfd,ldcs_fdlist_nt[fd].wd);

  return(rc);
 }

int ldcs_notify_get_fd(int fd) {
  int realfd=-1;
  if ((fd<0) || (fd>MAX_FD) )  _error("wrong fd");

  if(ldcs_fdlist_nt[fd].inuse) {
    realfd=ldcs_fdlist_nt[fd].ntfd;
  }
  debug_printf3("return ntfd=%d\n",realfd);

  return(realfd);
}

char *ldcs_notify_get_next_file(int fd) {
  char* result=NULL;

  struct inotify_event *event;

  if ((fd<0) || (fd>MAX_FD) )  _error("wrong fd");

  if ((ldcs_fdlist_nt[fd].buffer_next<0) 
      || (ldcs_fdlist_nt[fd].buffer_next>=ldcs_fdlist_nt[fd].buffer_enddata)) {
    ldcs_fdlist_nt[fd].buffer_next = 0;
    debug_printf3("read new events ...\n");
    ldcs_fdlist_nt[fd].buffer_enddata = 
      read (ldcs_fdlist_nt[fd].ntfd, ldcs_fdlist_nt[fd].buffer, ldcs_fdlist_nt[fd].buffer_len);
    debug_printf3("read new events ... %d bytes read\n",ldcs_fdlist_nt[fd].buffer_enddata);

    if (ldcs_fdlist_nt[fd].buffer_enddata < 0) {
      if (errno == EINTR) {
        ldcs_fdlist_nt[fd].buffer_enddata = 0;
        return NULL;
      } else {
	_error ("read on notify failed");
      }
    } else if (!ldcs_fdlist_nt[fd].buffer_enddata) {
      /* BUF_LEN too small? */
    }
  }
  
  debug_printf3("scan for next events ... buffer_next=%d\n",ldcs_fdlist_nt[fd].buffer_next);
  event = (struct inotify_event *) &ldcs_fdlist_nt[fd].buffer[ldcs_fdlist_nt[fd].buffer_next];

  debug_printf3("got event... wd=%d mask=%u cookie=%u len=%u \n",
	       event->wd, event->mask,event->cookie, event->len);

  if (event->len) {
    result=strdup(event->name);
    debug_printf3("          name=%s \n",event->name);
  }
    
  ldcs_fdlist_nt[fd].buffer_next += EVENT_SIZE + event->len;
  debug_printf3("shift buffer buffer_next=%d of enddata=%d event_size=%ld event_len=%u\n",
	       ldcs_fdlist_nt[fd].buffer_next,ldcs_fdlist_nt[fd].buffer_enddata,EVENT_SIZE,event->len);
  
  return(result);
}

int ldcs_notify_more_avail(int fd) {
  if ((fd<0) || (fd>MAX_FD) )  _error("wrong fd");
  
  return( (ldcs_fdlist_nt[fd].buffer_next>=ldcs_fdlist_nt[fd].buffer_enddata) ? 0 : 1);
}


