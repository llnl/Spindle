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

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <stdlib.h>
#include <time.h>

#include "config.h"
#include "client_heap.h"
#include "ldcs_api_pipe.h"
#include "ldcs_api.h"
#include "spindle_launch.h"

#define MAX_FD 1
static struct fdlist_entry_t fdlist_pipe[MAX_FD];

static int get_new_fd_pipe()
{
   /* Each client should establish one connection, just return 0 as our fd */
   return 0;
}

static int ldcs_mkfifo(char *fifo)
{
   int result;
   debug_printf3("mkfifo %s\n", fifo);
   result = mkfifo(fifo, 0600);    
   if (result == -1) {
      if (errno == EEXIST) {
         debug_printf2("Likely inheriting an existing pipe after exec\n");
         return -2;
      }
      err_printf("Error during mkfifo of %s: %s\n", fifo, strerror(errno));
      return -1;
   }

   return 0;
}

static int write_pipe(int fd, const void *data, int bytes)
{
  int left,bsumwrote;
  ssize_t bwrite, bwrote;
  char *dataptr;
  
  left      = bytes;
  bsumwrote = 0;
  dataptr   = (char*) data;

  while (left > 0) {
     bwrite     = left;
     bwrote     = write(fd, dataptr, bwrite);
     if (bwrote == -1) {
        if (errno == EINTR)
           continue;
        err_printf("Failed to wrote bytes to pipe: %s\n", strerror(errno));
        return -1;
     }
     left      -= bwrote;
     dataptr   += bwrote;
     bsumwrote += bwrote;
  }
  return bsumwrote;
}

static int read_pipe(int fd, void *data, int bytes)
{
   int         left;
   ssize_t     btoread, bread;
   char       *dataptr;
  
   left      = bytes;
   dataptr   = (char*) data;

   while (left > 0)  {
      btoread    = left;
      bread      = read(fd, dataptr, btoread);
      if (bread == -1) {
         if (errno == EINTR)
            continue;
         err_printf("Error reading data from pipe: %s\n", strerror(errno));
         return -1;
      }
      if (bread == 0) {
         err_printf("Server closed the pipe connection\n");
         return -1;
      }
      left      -= bread;
      dataptr   += bread;
   }
   return 0;
}
/* If we've just exec'd, we may have lost the fds for our pipe connection
   with the server.  Parse /proc/self/fd for the open fds and find the ones
   that point to the pipes.  This only works on Linux based systems with /proc
   mounted.  But, on the BlueGene alternative we won't be dealing with execs
   anyways.
*/
static int find_existing_fds(char *in_path, char *out_path, int *in_fd, int *out_fd)
{
   struct dirent *dent;
   DIR *dir;
   char fdpath[MAX_PATH_LEN+1];
   char dirpath[MAX_PATH_LEN+1];
   fdpath[MAX_PATH_LEN] = '\0';
   dirpath[MAX_PATH_LEN] = '\0';
   int found_in, found_out;

   dir = opendir("/proc/self/fd");
   if (!dir) {
      err_printf("Failed to open dir /proc/self/fd.  Perhaps /proc not mounted: %s\n", 
                 strerror(errno));
      return -1;
   }

   found_in = (in_path == NULL);
   found_out = (out_path == NULL);

   for (dent = readdir(dir); dent != NULL; dent = readdir(dir)) {
      int fd = atoi(dent->d_name);
      snprintf(dirpath, MAX_PATH_LEN, "/proc/self/fd/%d", fd);
      memset(fdpath, 0, sizeof(fdpath));
      (void)! readlink(dirpath, fdpath, MAX_PATH_LEN);

      debug_printf("Comparing fd %d of %s with in_path = %s, out_path = %s\n", fd, fdpath, in_path, out_path);
      if (!found_in && strcmp(fdpath, in_path) == 0) {
         *in_fd = fd;
         found_in = 1;
         continue;
      }
      if (!found_out && strcmp(fdpath, out_path) == 0) {
         *out_fd = fd;
         found_out = 1;
         continue;
      }
      if (found_in && found_out)
         break;
   }
   closedir(dir);

   if (!found_in || !found_out) {
      err_printf("Didn't find expected input/output fds: %d %d, %s %s\n",
                 found_in, found_out, in_path ? in_path : "NULL", out_path ? out_path : "NULL");
      return -1;
   }

   return 0;
}

#define MIN_REMAP_FD 315
#define MAX_REMAP_FD 315+1024
extern unsigned long opts;
static int remap_to_high_fd(int fd)
{  
#if !defined(MIN_REMAP_FD) || !defined(MAX_REMAP_FD)
   return fd;
#else
   int i;
   if (opts & OPT_NOHIDE)
      return fd;

   /* Find an unused fd */
   for (i = MIN_REMAP_FD; i < MAX_REMAP_FD; i++) {
      errno = 0;
      fcntl(i, F_GETFD);
      if (errno != EBADF)
         continue;
      dup2(fd, i);
      close(fd);
      debug_printf3("Remapped fd %d to high fd %d\n", fd, i);
      return i;
   }
   err_printf("Failed to map fd %d to higher limit\n", fd);
   return fd;
#endif
}

static long long now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long env_timelimit_ms(const char *name, long long default_ms)
{
   char *value = getenv(name);
   if (value && atoi(value) > 0)
      return 1000LL * atoi(value);
   return default_ms;
}

/* Timeout used for connecting to a server after the ready file has appeared */
static long long connect_timeout_ms(void)
{
   return env_timelimit_ms("COBO_CONNECT_TIMELIMIT", COBO_CONNECT_TIMELIMIT * 1000LL);
}

/* Timeout used when waiting for the server's ready file to appear */
static long long ready_timeout_ms(void)
{
#ifdef SPINDLE_READY_TIMELIMIT
   return env_timelimit_ms("SPINDLE_READY_TIMELIMIT", SPINDLE_READY_TIMELIMIT * 1000LL);
#else
   return env_timelimit_ms("SPINDLE_READY_TIMELIMIT", 4 * connect_timeout_ms());
#endif
}

int client_open_connection_pipe(char* location, number_t number)
{
   (void)number;
   int fd, result;
   struct stat st;
   int stat_cnt;
   char fifo[MAX_PATH_LEN];
   char ready[MAX_PATH_LEN];
   int find_r_fd = 0, find_w_fd = 0;
   int opened_in = 0, opened_out = 0;

   debug_printf("Client creating pipe for connection to server\n");
   fd = get_new_fd_pipe();
   if (fd < 0) 
      return -1;
  
   fdlist_pipe[fd].type = LDCS_PIPE_FD_TYPE_CONN;

   long long deadline = now_ms() + ready_timeout_ms();

   /* wait for the server to be ready */
   stat_cnt = 0;
   snprintf(ready, MAX_PATH_LEN, "%s/spindle_comm/ready", location);
   memset(&st, 0, sizeof(st));
   while ((stat(ready, &st) == -1) || ((st.st_mode & (S_IRUSR | S_IWUSR)) == 0)) {
      if (now_ms() >= deadline) {
         err_printf("Timed out waiting for server ready file %s\n", ready);
         return -1;
      }
      if (stat_cnt % 10 == 0)
         debug_printf3("waiting: location %s does not exists (after %d seconds)\n", ready, stat_cnt/10);
      usleep(100000); /* .1 seconds */
      stat_cnt++;
   }

   /* create incoming fifo; open the read end first */
   fdlist_pipe[fd].in_fd = fdlist_pipe[fd].out_fd = 0;
   fdlist_pipe[fd].out_fn = NULL;
   sprintf(fifo, "%s/spindle_comm/fifo-%d-0", location, getpid());
   result = ldcs_mkfifo(fifo);
   if (result == -2)
      find_r_fd = 1;
   else if (result == -1)
      return -1;
   fdlist_pipe[fd].in_fn = spindle_strdup(fifo);
   assert(fdlist_pipe[fd].in_fn);
   if (find_r_fd) {
      /* Fifo already exists.  Find an existing open fd. */
      debug_printf3("Finding existing fd for %s\n", fdlist_pipe[fd].in_fn);
      find_existing_fds(fdlist_pipe[fd].in_fn, NULL, &fdlist_pipe[fd].in_fd, &fdlist_pipe[fd].out_fd);
   }
   if (fdlist_pipe[fd].in_fd == 0) {
      debug_printf3("Opening input fifo %s\n", fdlist_pipe[fd].in_fn);
      fdlist_pipe[fd].in_fd = open(fdlist_pipe[fd].in_fn, O_RDONLY | O_NONBLOCK);
      if (fdlist_pipe[fd].in_fd == -1) {
         err_printf("Error opening input fifo %s: %s\n", fdlist_pipe[fd].in_fn, strerror(errno));
         goto fail;
      }
      opened_in = 1;
   }

   /* create outgoing fifo */
   sprintf(fifo, "%s/spindle_comm/fifo-%d-1", location, getpid());
   result = ldcs_mkfifo(fifo);
   if (result == -2)
      find_w_fd = 1;
   else if (result == -1)
      goto fail;
   fdlist_pipe[fd].out_fn = spindle_strdup(fifo);

   if (find_w_fd) {
      /* Fifo already exists.  Find an existing open fd. */
      debug_printf3("Finding existing fd for %s\n", fdlist_pipe[fd].out_fn);
      find_existing_fds(NULL, fdlist_pipe[fd].out_fn, &fdlist_pipe[fd].in_fd, &fdlist_pipe[fd].out_fd);
   }
   if (fdlist_pipe[fd].out_fd == 0) {
      /* will get ENXIO until the server has opened its read end */
      debug_printf3("Opening output fifo %s\n", fdlist_pipe[fd].out_fn);
      deadline = now_ms() + connect_timeout_ms();
      for (;;) {
         fdlist_pipe[fd].out_fd = open(fdlist_pipe[fd].out_fn, O_WRONLY | O_NONBLOCK);
         if (fdlist_pipe[fd].out_fd != -1 || errno != ENXIO)
            break;
         if (now_ms() >= deadline) {
            err_printf("Timed out waiting for server to open %s\n", fdlist_pipe[fd].out_fn);
            goto fail;
         }
         usleep(10000);
      }
      if (fdlist_pipe[fd].out_fd == -1) {
         err_printf("Error opening output fifo %s: %s\n", fdlist_pipe[fd].out_fn, strerror(errno));
         goto fail;
      }
      opened_out = 1;
   }
   /* Turn nonblocking IO on the fifos off so as to not alter behavior after startup */
   fcntl(fdlist_pipe[fd].in_fd, F_SETFL, fcntl(fdlist_pipe[fd].in_fd, F_GETFL) & ~O_NONBLOCK);
   fcntl(fdlist_pipe[fd].out_fd, F_SETFL, fcntl(fdlist_pipe[fd].out_fd, F_GETFL) & ~O_NONBLOCK);
   if (opened_in)
      fdlist_pipe[fd].in_fd = remap_to_high_fd(fdlist_pipe[fd].in_fd);
   if (opened_out)
      fdlist_pipe[fd].out_fd = remap_to_high_fd(fdlist_pipe[fd].out_fd);
   debug_printf3("Opened fifos %s = %d, %s = %d\n", fdlist_pipe[fd].in_fn, fdlist_pipe[fd].in_fd,
                 fdlist_pipe[fd].out_fn, fdlist_pipe[fd].out_fd);

   return fd;

 fail:
   /* on failure, close our fds/fifos so the server will
      drop this connection if it tries to use them */
   if (opened_in)
      close(fdlist_pipe[fd].in_fd);
   if (opened_out)
      close(fdlist_pipe[fd].out_fd);
   if (!find_r_fd)
      unlink(fdlist_pipe[fd].in_fn);
   if (!find_w_fd && fdlist_pipe[fd].out_fn)
      unlink(fdlist_pipe[fd].out_fn);
   return -1;
}

int client_register_connection_pipe(char *connection_str)
{
   char *in_name = NULL, *out_name = NULL;
   int in_fd, out_fd, result;

   result = sscanf(connection_str, "%ms %ms %d %d", &in_name, &out_name, &in_fd, &out_fd);
   if (result != 4) {
      err_printf("Reading connection string.  Returned %d on '%s' (%s %s %d %d)\n", result, connection_str,
                 in_name, out_name, in_fd, out_fd);
      return -1;
   }

   int fd = get_new_fd_pipe();
   if (fd < 0) {
      err_printf("Could not create new pipe\n");
      return -1;
   }

   fdlist_pipe[fd].type = LDCS_PIPE_FD_TYPE_CONN;
   fdlist_pipe[fd].in_fn = in_name;
   fdlist_pipe[fd].out_fn = out_name;
   fdlist_pipe[fd].in_fd = in_fd;
   fdlist_pipe[fd].out_fd = out_fd;

   return fd;
}

char *client_get_connection_string_pipe(int fd)
{
   char *in_name = fdlist_pipe[fd].in_fn;
   char *out_name = fdlist_pipe[fd].out_fn;
   int in_fd = fdlist_pipe[fd].in_fd;
   int out_fd = fdlist_pipe[fd].out_fd;
   
   int slen = strlen(in_name) + strlen(out_name) + 64;
   char *str = (char *) spindle_malloc(slen);
   if (!str)
      return NULL;
   snprintf(str, slen, "%s %s %d %d", in_name, out_name, in_fd, out_fd);
   return str;
}

int client_send_msg_pipe(int fd, ldcs_message_t *msg) {

   int result;

   assert(fd >= 0 && fd < MAX_FD);
   
   debug_printf3("sending message of size len=%lu\n", msg->header.len);
   
   result = write_pipe(fdlist_pipe[fd].out_fd, &msg->header, sizeof(msg->header));
   if (result == -1)
      return -1;

   if (msg->header.len == 0)
      return 0;

   result = write_pipe(fdlist_pipe[fd].out_fd, (void *) msg->data, msg->header.len);
   if (result == -1)
      return -1;
    
   return 0;
}

static int client_recv_msg_pipe(int fd, ldcs_message_t *msg, ldcs_read_block_t block, int is_dynamic)
{
   int result;
   msg->header.type=LDCS_MSG_UNKNOWN;
   msg->header.len=0;

   assert(fd >= 0 && fd < MAX_FD);
   assert(block == LDCS_READ_BLOCK); /* Non-blocking isn't implemented yet */

   debug_printf3("Reading %d bytes for header from pipe\n", (int) sizeof(msg->header));
   result = read_pipe(fdlist_pipe[fd].in_fd, &msg->header, sizeof(msg->header));
   if (result == -1) {
      return -1;
   }
   
   if (msg->header.len == 0) {
      msg->data = NULL;
      return 0;
   }

   if (is_dynamic) {
      msg->data = (char *) spindle_malloc(msg->header.len);
   }

   debug_printf3("Reading %lu bytes for payload from pipe\n", msg->header.len);
   result = read_pipe(fdlist_pipe[fd].in_fd, msg->data, msg->header.len);
   return result;
}

int client_recv_msg_dynamic_pipe(int fd, ldcs_message_t *msg, ldcs_read_block_t block)
{
   return client_recv_msg_pipe(fd, msg, block, 1);
}

int client_recv_msg_static_pipe(int fd, ldcs_message_t *msg, ldcs_read_block_t block)
{
   return client_recv_msg_pipe(fd, msg, block, 0);
}

int is_client_fd(int connfd, int fd)
{
   return (fdlist_pipe[connfd].in_fd == fd || fdlist_pipe[connfd].out_fd == fd);
}

/* Get raw FDs for use in signal handlers.
   We can't use the send/recv functions here in signal handlers
   because they are not async-signal-safe. */
int client_get_raw_fds_pipe(int fd, int *read_fd, int *write_fd)
{
   if (fd < 0 || fd >= MAX_FD) {
      *read_fd = -1;
      *write_fd = -1;
      return -1;
   }
   *read_fd = fdlist_pipe[fd].in_fd;
   *write_fd = fdlist_pipe[fd].out_fd;
   return 0;
}

int client_close_connection_pipe(int fd)
{
   int result;

   assert(fd >= 0 && fd < MAX_FD);

   debug_printf2("Closing client connections.  Closing FDs on input %s (%d) and output %s (%d)\n",
                 fdlist_pipe[fd].in_fn, fdlist_pipe[fd].in_fd, fdlist_pipe[fd].out_fn, fdlist_pipe[fd].out_fd);

   result = close(fdlist_pipe[fd].in_fd);
   if(result != 0) {
      err_printf("Error while closing fifo %s errno=%d (%s)\n", fdlist_pipe[fd].in_fn, errno, strerror(errno));
   }
   result = close(fdlist_pipe[fd].out_fd);
   if(result != 0) {
      err_printf("Error while closing fifo %s errno=%d (%s)\n", fdlist_pipe[fd].out_fn, errno, strerror(errno));
   }

   return 0;
}
