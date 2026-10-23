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

#include "config.h"
#include "ldcs_api.h"
#include "spindle_debug.h"
#include "keyfile.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#if defined(KEYFILE) && !defined(SEC_KEYDIR)
#error Spindle built with sec-keyfile, but no keyfile specified
#endif

extern char *parse_location(char *loc, number_t number);
void get_keyfile_path(char *pathname, int pathname_len, uint64_t unique_id)
{
#if defined(KEYFILE)
   debug_printf("Turning pathname %s and unique_id %lu to key file location\n",
                SEC_KEYDIR, (unsigned long) unique_id);

   char *demangled_loc = parse_location(SEC_KEYDIR, 0);
   if (demangled_loc == NULL)
      abort();
   snprintf(pathname, pathname_len, "%s/spindle_key.%d.%lu", demangled_loc, getuid(), (unsigned long) unique_id);
   pathname[pathname_len-1] = '\0';
#else
   (void)pathname;
   (void)pathname_len;
   (void)unique_id;
   assert(0 && "Tried to use keyfile when not compiled with keyfile.");
#endif
}

void create_key(unsigned char *buffer, int key_size_bytes)
{
   int rand_fd;
   int result;
   int bytes_read = 0;

   debug_printf("Generating key from /dev/urandom\n");

   rand_fd = open("/dev/urandom", O_RDONLY);
   if (rand_fd == -1) {
      fprintf(stderr, "Failed to open /dev/urandom for key creation\n");
      exit(-1);
   }

   do {
      result = read(rand_fd, buffer + bytes_read, key_size_bytes - bytes_read);
      if (result == -1 && errno == EINTR)
         continue;
      if (result <= 0) {
         fprintf(stderr, "Failed to read bytes from /dev/urandom for key generation\n");
         exit(-1);
      }
      bytes_read += result;
   } while (bytes_read < key_size_bytes);
   close(rand_fd);
}

void create_keyfile(uint64_t unique_id)
{
   char path[MAX_PATH_LEN+1];
   int key_fd, result;
   size_t bytes_written;
   char *last_slash;
   struct stat buf;
   unsigned char key[KEY_SIZE_BYTES];

   create_key(key, sizeof(key));
   
   get_keyfile_path(path, sizeof(path), unique_id);
   
   last_slash = strrchr(path, '/');
   *last_slash = '\0';
   if (stat(path, &buf) == -1 && errno == ENOENT) {
      debug_printf("Creating directory for keyfile: %s\n", path);
      result = mkdir(path, 0700);
      if (result == -1) {
         fprintf(stderr, "Failed to create directory for spindle key: %s\n", path);
         exit(-1);
      }
   }
   *last_slash = '/';

   debug_printf("Creating keyfile at %s\n", path);
   key_fd = open(path, O_CREAT|O_EXCL|O_WRONLY, 0600);
   if (key_fd == -1) {
      fprintf(stderr, "Failed to create security keyfile %s: %s\n", path, strerror(errno));
      exit(-1);
   }

   bytes_written = 0;
   do {
      result = write(key_fd, key + bytes_written, sizeof(key) - bytes_written);
      if (result == -1 && errno == EINTR)
         continue;
      if (result <= 0) {
         fprintf(stderr, "Failed to write key to %s: %s\n", path, strerror(errno));
         close(key_fd);
         clean_keyfile(unique_id);
         exit(-1);
      }
      bytes_written += result;
   } while (bytes_written < sizeof(key));
   
   result = close(key_fd);
   if (result == -1) {
      clean_keyfile(unique_id);
      fprintf(stderr, "Failed to close key file %s: %s\n", path, strerror(errno));
      exit(-1);
   }

   debug_printf("Finished creating keyfile\n");
}

void clean_keyfile(uint64_t unique_id)
{
   char path[MAX_PATH_LEN+1];
   get_keyfile_path(path, sizeof(path), unique_id);
   unlink(path);
   debug_printf("Cleaned keyfile %s\n", path);
}

const char* get_sec_keydir_path(void)
{
#if defined(KEYFILE) && defined(SEC_KEYDIR)
   return SEC_KEYDIR;
#else
   return NULL;
#endif
}
