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

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#if !defined(USE_PLUGIN_DEBUG)
#include "spindle_debug.h"
#else
#include "plugin_utils.h"
#define SPINDLE_DEBUG_H_
#endif

#include "config.h"

#if defined(TRACK_MKDIR)
extern void track_mkdir(const char *dir);
#endif
#if defined(LOOKUP_PREV_MKDIR)
extern int lookup_prev_mkdir(const char *dir);
#endif

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int spindle_mkdir_lstat(const char *path, struct stat *buf)
{
   return lstat(path, buf);
}

static ssize_t spindle_mkdir_readlink(const char *path, char *buf, size_t size)
{
   return readlink(path, buf, size);
}

static int spindle_mkdir_mkdir(const char *path, mode_t mode)
{
   return mkdir(path, mode);
}

static int within(const char *path, const char *prefix, size_t length)
{
   return !strncmp(path, prefix, length) &&
      (length == 1 || path[length] == '/' || path[length] == '\0');
}

static int append(char *path, size_t *length, const char *part, size_t size)
{
   size_t separator = !(*length == 1 && path[0] == '/');

   if (*length + separator > PATH_MAX - 1 ||
       size > PATH_MAX - 1 - *length - separator) {
      errno = ENAMETOOLONG;
      debug_printf("WARNING: append failing with ENAMETOLONG on '%s' and '%s'\n", path, part);
      return -1;
   }
   if (separator)
      path[(*length)++] = '/';
   memcpy(path + *length, part, size);
   *length += size;
   path[*length] = '\0';
   return 0;
}

/* Move up from a resolved path. Relative paths retain leading '..' because
 * the current directory cannot be named absolutely with the allowed calls. */
static int parent(char path[PATH_MAX], size_t *length)
{
   if (path[0] != '/' &&
       ((*length == 1 && path[0] == '.') ||
        (*length >= 3 && path[*length - 3] == '/' &&
         path[*length - 2] == '.' && path[*length - 1] == '.')))
      return append(path, length, "..", 2);

   while (*length > 1 && path[*length - 1] != '/')
      --*length;
   if (*length > 1)
      --*length;
   path[*length] = '\0';
   return 0;
}

static int check_directory(const char *path, struct stat *existing_stat)
{
   struct stat st;
   int error;
   
   if (!existing_stat) {
      if (spindle_mkdir_lstat(path, &st) < 0) {
         error = errno;
         debug_printf("WARNING: Failed to stat %s: %s\n", path, strerror(error));
         errno = error;
         return -1;
      }
      existing_stat = &st;
   }
   if (!S_ISDIR(existing_stat->st_mode)) {
      errno = ENOTDIR;
      debug_printf("WARNING: %s is not a directory\n", path);
      return -1;
   }
   if (existing_stat->st_uid != geteuid() || existing_stat->st_gid != getegid() ||
       (existing_stat->st_mode & 07777) != 0700) {
      errno = EACCES;
      debug_printf("WARNING: Permissions error on %s\n", path);
      return -1;
   }
   return 0;
}

/* EEXIST may come from another process; verify what is there either way. */
static int make_directory(const char *path)
{
   int result;
   if (spindle_mkdir_mkdir(path, 0700) < 0 && errno != EEXIST) {
      debug_printf("WARNING: Failed to mkdir(%s)\n", path);
      return -1;
   }
   result = check_directory(path, NULL);
   if (result == -1) {
      debug_printf("WARNING: check_directory failed here\n");
      return -1;
   }
#if defined(TRACK_MKDIR)
   track_mkdir(path);
#endif
   return result;
}

/* Process the path 'suffix' and make new directories. path starts at "/", ".", or the already
   resolved cachepath. ../, ./, and symlinks are resolved and, if a cachepath is provided,
   must stay within the cachepath. */
static int mkdir_impl(const char *suffix, const char *cachepath, char path[PATH_MAX])
{
   char pending[PATH_MAX], target[PATH_MAX];
   size_t length, cachepath_length;
   size_t cursor = 0, pending_length;
   int links = 0, result, error;

   length = strlen(path);
   cachepath_length = cachepath ? strlen(cachepath) : 0;
   pending_length = strlen(suffix);
   if (pending_length >= sizeof(pending)) {
      debug_printf("WARNING: mkdir_impl ENAMETOLONG on '%s'\n", suffix);
      errno = ENAMETOOLONG;
      return -1;
   }
   /* Expansion can insert NUL separators, so pending_length, not strlen,
    * defines the part of pending still in use. */
   memcpy(pending, suffix, pending_length);

   while (cursor < pending_length) {
      size_t start, end, old_length;
      size_t boundary;
      struct stat st;
      ssize_t symlink_target_size;

      /* Skip empty components. An inserted NUL ends one symlink target;
       * check it before applying the caller's remaining components. */
      while (cursor < pending_length && pending[cursor] == '/')
         ++cursor;
      if (cursor == pending_length)
         break;

      /* A NUL marks the end of a symlink target, before its caller's tail. */
      if (pending[cursor] == '\0') {
         if (cachepath && !within(path, cachepath, cachepath_length)) {
            debug_printf("WARNING within failing with EACCES on '%s' in '%s'\n", path, cachepath);
            errno = EACCES;
            return -1;
         }
         ++cursor;
         continue;
      }
      
      start = cursor;
      while (cursor < pending_length && pending[cursor] && pending[cursor] != '/')
         ++cursor;
      end = cursor;

      /* Path component is a './'. Move on. */
      if (end - start == 1 && pending[start] == '.') {
         continue;
      }

      /* Path component is a '../'. Use parent() to move up a directory as long, as we're not on the
         cachepath root */
      if (end - start == 2 && pending[start] == '.' && pending[start + 1] == '.') {
         /* Clamp at cachepath, just as '..' at '/' stays at '/'. */
         if (!cachepath || length > cachepath_length) {
            result = parent(path, &length);            
            if (result < 0) {
               error = errno;
               debug_printf("WARNING mkdir_impl failing because could not determine parent of '%s' with cachepath '%s'\n",
                             path ? path : "[NULL]", cachepath ? cachepath : "[NULL]");
               errno = error;
               return -1;
            }
         }
         continue;
      }

      old_length = length;
      if (append(path, &length, pending + start, end - start) < 0)
         return -1;
      /* Missing components are kept in the resolved path for the later
       * mkdir pass. Existing non-directories cannot be traversed. */
      result = spindle_mkdir_lstat(path, &st);

      /* Create the directory if the path component does not exist */
      if (result == -1 && errno == ENOENT) {
         result = make_directory(path);
         if (result == -1) {
            return -1;
         }
         continue;
      }      
      if (result == -1) {
         error = errno;
         debug_printf("WARNING: lstat failed during mkdir_impl for '%s': %s\n", path, strerror(error));
         errno = error;
         return -1;
      }
      if (S_ISDIR(st.st_mode)) {
         if (cachepath && length > cachepath_length) {
            /* We're strictly underneath the cachepath on an existing directory. Permissions should be locked down. */
            result = check_directory(path, &st);
            if (result == -1) {
               error = errno;
               debug_printf("WARNING: Permission error underneath cachepath %s: %s\n", path, strerror(error));
               errno = error;
               return -1;
            }
         }
         continue;
      }
      if (!S_ISLNK(st.st_mode)) {
         debug_printf("WARNING: mkdir_impl failing for '%s' because ENOTDIR\n", path);
         errno = ENOTDIR;
         return -1;
      }
      
      /* Check targets of links inside the prefix, even if the caller's
       * remaining path would later return inside. Links in a prefix alias
       * may resolve to an ancestor, so they get no boundary check here. */
      boundary = cachepath && within(path, cachepath, cachepath_length);
      if (++links > 40) {
         debug_printf("WARNING: mkdir_impl failing for '%s' because ELOOP\n", path);
         errno = ELOOP;
         return -1;
      }
      symlink_target_size = spindle_mkdir_readlink(path, target, sizeof(target) - 1);
      if (symlink_target_size < 0) {
         error = errno;
         debug_printf("WARNING: mkdir_impl failing for '%s' because readlink failed: %s\n", path, strerror(error));
         errno = error;
         return -1;
      }
      if (symlink_target_size == 0) {
         debug_printf("WARNING: mkdir_impl failing for '%s' with ENOENT\n", path);
         errno = ENOENT;
         return -1;
      }
      if ((size_t)symlink_target_size + boundary + pending_length - end >= sizeof(pending)) {
         debug_printf("WARNING: mkdir_impl failing for '%s' with ENAMETOOLONG\n", path);
         errno = ENAMETOOLONG;
         return -1;
      }
      
      /* Replace the link with its target and keep the unread tail. The
       * optional NUL marks where this target ends. Nested expansions keep
       * earlier markers in that tail without a separate stack. */
      memmove(pending + symlink_target_size + boundary, pending + end, pending_length - end);
      memcpy(pending, target, symlink_target_size);
      if (boundary)
         pending[symlink_target_size] = '\0';
      pending_length = (size_t) symlink_target_size + boundary + pending_length - end;
      cursor = 0;
      /* Absolute targets restart at root; relative targets restart at the
       * link's parent, saved in old_length. */
      if (target[0] == '/') {
         path[0] = '/';
         path[1] = '\0';
         length = 1;
      } else {
         length = old_length;
         path[length] = '\0';
      }
   }
   /* Also reject an ordinary path that ends outside the prefix. */
   if (cachepath && !within(path, cachepath, cachepath_length)) {
      debug_printf("WARNING: mkdir_impl failing for '%s' with EACCESs and cachepath of %s\n", path, cachepath);
      errno = EACCES;
      return -1;
   }
   return 0;
}

static ssize_t validate_path_under_cachepath(const char *path, const char *cachepath)
{
   ssize_t prefix_length;
   prefix_length = strlen(cachepath);
   while (prefix_length > 1 && cachepath[prefix_length - 1] == '/')
      --prefix_length;
   if (prefix_length >= PATH_MAX) {
      debug_printf("WARNING: spindle_mkdir failing with ENAMETOOLONG on '%s'\n",
                    path ? path : "[NULL]");
      errno = ENAMETOOLONG;
      return -1;
   }
   if (!within(path, cachepath, prefix_length)) {
      debug_printf("WARNING: spindle_mkdir failing with EACCESS on '%s' and '%s'\n",
                    path ? path : "[NULL]", cachepath ? cachepath : "[NULL]");
      errno = EACCES;
      return -1;
   }
   return prefix_length;
}

static int check_mkdir_args(const char *path, const char *cachepath)
{
   if (!path || !*path || (cachepath && (cachepath[0] != '/' || path[0] != '/'))) {
      debug_printf("WARNING: spindle_mkdir failing with EINVAL on '%s' with cachepath '%s'\n",
                    path ? path : "[NULL]", cachepath ? cachepath : "[NULL]");
      errno = EINVAL;
      return -1;
   }
   return 0;
}

static int spindle_mkdir_impl(const char *path, const char *cachepath)
{
   char resolved[PATH_MAX];
   ssize_t prefix_length;
   int error;

   if (check_mkdir_args(path, cachepath) == -1)
      return -1;

   if (cachepath) {
      /* Validate that path is prefixed by the cachepath */
      prefix_length = validate_path_under_cachepath(path, cachepath);
      if (prefix_length == -1) {
         error = errno;
         debug_printf("WARNING: Failed to validate that mkdir cache is under cachepath '%s' and '%s'\n",
                       path, cachepath);
         errno = error;
         return -1;
      }     

      strncpy(resolved, cachepath, sizeof(resolved));
      if (mkdir_impl(path + prefix_length, cachepath, resolved) < 0) {
         return -1;
      }
   } else {
      /* Relative paths stay relative; absolute paths start at root. */
      strcpy(resolved, path[0] == '/' ? "/" : ".");
      if (mkdir_impl(path, NULL, resolved) < 0) {
         return -1;
      }
   }

   return 0;
}

int spindle_mkdir(const char *path)
{
   char resolved[PATH_MAX];

   if (check_mkdir_args(path, NULL) == -1)
      return -1;

   strcpy(resolved, path[0] == '/' ? "/" : ".");
   if (mkdir_impl(path, NULL, resolved) < 0) {
      return -1;
   }
   
   return 0;
}

int spindle_mkdir_in_cache(const char *path, const char *cachepath)
{
   char resolved[PATH_MAX];
   ssize_t prefix_length;
   int error;

   if (!cachepath)
      return spindle_mkdir(path);

   if (check_mkdir_args(path, cachepath) == -1)
      return -1;

   /* Validate that path is prefixed by the cachepath */
   prefix_length = validate_path_under_cachepath(path, cachepath);
   if (prefix_length == -1) {
      error = errno;
      debug_printf("WARNING: Failed to validate that mkdir on '%s' is under cachepath '%s'\n",
                   path, cachepath);
      errno = error;
      return -1;
   }     
   
   strncpy(resolved, cachepath, sizeof(resolved));
   if (mkdir_impl(path + prefix_length, cachepath, resolved) < 0) {
      return -1;
   }

   return 0;
}
