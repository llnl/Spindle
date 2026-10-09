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
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include "ccwarns.h"

#if !defined(RPTEST)
#include "intercept.h"
#include "client.h"
#include "spindle_debug.h"
#endif

static int spindlerp_readlink(const char *path, char *buf, size_t bufsiz)
{
#if defined(RPTEST)
   return readlink(path, buf, bufsiz);
#else
   return readlink_wrapper(path, buf, bufsiz);
#endif
}

static int spindlerp_stat(const char *pathname, struct stat *statbuf)
{
#if defined(RPTEST)
   return stat(pathname, statbuf);
#else
   return rtcache_stat(pathname, statbuf);
#endif
}

static void spindlerp_set_errno(int error)
{
#if !defined(RPTEST)
   set_errno(error);
#endif
   errno = error;
}

//Uses spindle namespace malloc
static void *spindlerp_int_malloc(size_t size)
{
   return malloc(size);
}

static void spindlerp_int_free(void *p)
{
   free(p);
}

char *spindlerp_getcwd(char *buf, size_t size)
{
   return getcwd(buf, size);
}

//Uses application namespace malloc
static char *spindlerp_ext_strdup(const char *s)
{
#if defined(RPTEST)
   return strdup(s);
#else
   char *buf;
   size_t len;
   malloc_sig_t mallocf;

   mallocf = get_app_malloc();
   if (!mallocf) {
      err_printf("Could not lookup malloc function for realpath result\n");
      return NULL;
   }
   len = strlen(s) + 1;
   buf = (char *) mallocf(len);
   if (!buf) {
      err_printf("realpath malloc failure\n");
      return NULL;      
   }
   memcpy(buf, s, len);   
   return buf;
#endif
}

char* (*orig_realpath)(const char *, char *);

/*
The contents of this file were copied and modified from GLIBC's realpath() implementation.
It is a derivavitve work. Since we are both LGPL, this is allowed.

It has been significantly modified to:
A. Work outside of glibc
B. Use spindle versions of the IO accessing lower-level routines.

The source file is glibc/stdlib/canonicalize.c at git commit 1894e219dc530d7074085e95ffe3c1e66cebc072
 */

#define ISSLASH(c) ((c) == '/')
#define PATH_MAX 4096
#define IS_ABSOLUTE_FILE_NAME(name) (ISSLASH((name)[0]))
#define MIN_ELOOP_THRESHOLD    40

/* Return true if FILE's existence can be shown, false (setting errno)
   otherwise.  Follow symbolic links.  */
static int file_accessible (char const *file)
{
   struct stat st;
   return spindlerp_stat (file, &st) == 0 || errno == EOVERFLOW;
}

/* Scratch buffer.  Must be initialized with scratch_buffer_init
   before its use.  */
struct scratch_buffer {
  void *data;    /* Pointer to the beginning of the scratch area.  */
  size_t length; /* Allocated space at the data pointer, in bytes.  */
  union { void* __align; char __c[1024]; } __space;
};
/* Initializes *BUFFER so that BUFFER->data points to BUFFER->__space
   and BUFFER->length reflects the available space.  */
static inline void
scratch_buffer_init (struct scratch_buffer *buffer)
{
  buffer->data = buffer->__space.__c;
  buffer->length = sizeof (buffer->__space);
}
/* Deallocates *BUFFER (if it was heap-allocated).  */
static inline void
scratch_buffer_free (struct scratch_buffer *buffer)
{
  if (buffer->data != buffer->__space.__c)
     spindlerp_int_free (buffer->data);
}

/* Alias for __libc_scratch_buffer_grow.  */
static int
scratch_buffer_grow (struct scratch_buffer *buffer)
{
  void *new_ptr;
  size_t new_length = buffer->length * 2;

  /* Discard old buffer.  */
  scratch_buffer_free (buffer);

  /* Check for overflow.  */
  if (new_length >= buffer->length)
    new_ptr = spindlerp_int_malloc (new_length);
  else
  {
     spindlerp_set_errno (ENOMEM);
     new_ptr = NULL;
  }

  if (new_ptr == NULL)
  {
     /* Buffer must remain valid to free.  */
     scratch_buffer_init (buffer);
     return 0;
  }

  /* Install new heap-based buffer.  */
  buffer->data = new_ptr;
  buffer->length = new_length;
  return 1;
}
/* Like __libc_scratch_buffer_grow, but preserve the old buffer
   contents on success, as a prefix of the new buffer.  */
static int
scratch_buffer_grow_preserve (struct scratch_buffer *buffer)
{
   size_t new_length = 2 * buffer->length;
   void *new_ptr;

   if (buffer->data == buffer->__space.__c)
   {
      /* Move buffer to the heap.  No overflow is possible because
         buffer->length describes a small buffer on the stack.  */
      new_ptr = spindlerp_int_malloc (new_length);
      if (new_ptr == NULL)
         return 0;
      memcpy (new_ptr, buffer->__space.__c, buffer->length);
   }
   else
   {
      /* Buffer was already on the heap.  Check for overflow.  */
      if (new_length >= buffer->length)
         new_ptr = realloc (buffer->data, new_length);
      else
      {
         spindlerp_set_errno (ENOMEM);
         new_ptr = NULL;
      }

      if (new_ptr == NULL)
      {
         /* Deallocate, but buffer must remain valid to free.  */
         spindlerp_int_free (buffer->data);
         scratch_buffer_init (buffer);
         return 0;
      }
   }

   /* Install new heap-based buffer.  */
   buffer->data = new_ptr;
   buffer->length = new_length;
   return 1;
}

/* True if concatenating END as a suffix to a file name means that the
   code needs to check that the file name is that of a searchable
   directory, since the canonicalize_filename_mode_stk code won't
   check this later anyway when it checks an ordinary file name
   component within END.  END must either be empty, or start with a
   slash.  */
static int
suffix_requires_dir_check (char const *end)
{
   /* If END does not start with a slash, the suffix is OK.  */
   while (ISSLASH (*end))
   {
      /* Two or more slashes act like a single slash.  */
      do
         end++;
      while (ISSLASH (*end));

      switch (*end++)
      {
         default: return 0;  /* An ordinary file name component is OK.  */
         case '\0': return 1; /* Trailing "/" is trouble.  */
         case '.': break;        /* Possibly "." or "..".  */
      }
      /* Trailing "/.", or "/.." even if not trailing, is trouble.  */
      if (!*end || (*end == '.' && (!end[1] || ISSLASH (end[1]))))
         return 1;
   }

   return 0;
}

static char const dir_suffix[] = "/";

/* Return true if DIR is a searchable dir, false (setting errno) otherwise.
   DIREND points to the NUL byte at the end of the DIR string.
   Store garbage into DIREND[0 .. strlen (dir_suffix)].  */

static int
dir_check (char *dir, char *dirend)
{
   strcpy (dirend, dir_suffix);
   return file_accessible (dir);
}

static long
get_path_max (void)
{
   return 4096;
}

/* Scratch buffers used by realpath_stk and managed by __realpath.  */
struct realpath_bufs
{
   struct scratch_buffer rname;
   struct scratch_buffer extra;
   struct scratch_buffer link;
};

static char *
realpath_stk (const char *name, char *resolved, struct realpath_bufs *bufs)
{
   char *dest;
   char const *start;
   char const *end;
   int num_links = 0;
   char *result;

   if (name == NULL)
   {
      /* As per Single Unix Specification V2 we must return an error if
         either parameter is a null pointer.  We extend this to allow
         the RESOLVED parameter to be NULL in case the we are expected to
         allocate the room for the return value.  */
      spindlerp_set_errno (EINVAL);
      return NULL;
   }

   if (name[0] == '\0')
   {
      /* As per Single Unix Specification V2 we must return an error if
         the name argument points to an empty string.  */
      spindlerp_set_errno (ENOENT);
      return NULL;
   }

   char *rname = bufs->rname.data;
   int end_in_extra_buffer = 0;
   int failed = 1;

   /* This is always zero for Posix hosts, but can be 2 for MS-Windows
      and MS-DOS X:/foo/bar file names.  */
   long prefix_len = 0;

   if (!IS_ABSOLUTE_FILE_NAME (name))
   {
      while (!spindlerp_getcwd (bufs->rname.data, bufs->rname.length))
      {
         if (errno != ERANGE)
         {
            dest = rname;
            goto error;
         }
         if (!scratch_buffer_grow (&bufs->rname))
            return NULL;
         rname = bufs->rname.data;
      }
      dest = strchr (rname, '\0');
      start = name;
      prefix_len = 0;
   }
   else
   {
      dest = mempcpy (rname, name, prefix_len);
      *dest++ = '/';
      start = name + prefix_len;
   }
   for ( ; *start; start = end)
   {
      /* Skip sequence of multiple file name separators.  */
      while (ISSLASH (*start))
         ++start;

      /* Find end of component.  */
      for (end = start; *end && !ISSLASH (*end); ++end)
         /* Nothing.  */;

      /* Length of this file name component; it can be zero if a file
         name ends in '/'.  */
      long startlen = end - start;

      if (startlen == 0)
         break;
      else if (startlen == 1 && start[0] == '.')
         /* nothing */;
      else if (startlen == 2 && start[0] == '.' && start[1] == '.')
      {
         /* Back up to previous component, ignore if at root already.  */
         if (dest > rname + prefix_len + 1)
            for (--dest; dest > rname && !ISSLASH (dest[-1]); --dest)
               continue;
      }
      else
      {
         if (!ISSLASH (dest[-1]))
            *dest++ = '/';

         GCC_DISABLE_WARNING("-Wsign-compare")  // long int vs long unsigned int
         while (rname + bufs->rname.length - dest
                < startlen + sizeof dir_suffix)
         {
            long dest_offset = dest - rname;
            if (!scratch_buffer_grow_preserve (&bufs->rname))
               return NULL;
            rname = bufs->rname.data;
            dest = rname + dest_offset;
         }
         GCC_ENABLE_WARNING

         dest = mempcpy (dest, start, startlen);
         *dest = '\0';

         char *buf;
         ssize_t n;
         while (1)
         {
            buf = bufs->link.data;
            long bufsize = bufs->link.length;
            n = spindlerp_readlink (rname, buf, bufsize - 1);
            if (n < bufsize - 1)
               break;
            if (!scratch_buffer_grow (&bufs->link))
               return NULL;
         }
         if (0 <= n)
         {
            if (++num_links > MIN_ELOOP_THRESHOLD)
            {
               spindlerp_set_errno (ELOOP);
               goto error;
            }

            buf[n] = '\0';

            char *extra_buf = bufs->extra.data;
            long end_idx = 0;
            if (end_in_extra_buffer)
               end_idx = end - extra_buf;
            size_t len = strlen (end);
            while (bufs->extra.length <= len + n)
            {
               if (!scratch_buffer_grow_preserve (&bufs->extra))
                  return NULL;
               extra_buf = bufs->extra.data;
            }
            if (end_in_extra_buffer)
               end = extra_buf + end_idx;

            /* Careful here, end may be a pointer into extra_buf... */
            memmove (&extra_buf[n], end, len + 1);
            name = end = memcpy (extra_buf, buf, n);
            end_in_extra_buffer = 1;

            if (IS_ABSOLUTE_FILE_NAME (buf))
            {
               long pfxlen = 0;

               dest = mempcpy (rname, buf, pfxlen);
               *dest++ = '/'; /* It's an absolute symlink */
               /* Install the new prefix to be in effect hereafter.  */
               prefix_len = pfxlen;
            }
            else
            {
               /* Back up to previous component, ignore if at root
                  already: */
               if (dest > rname + prefix_len + 1)
                  for (--dest; dest > rname && !ISSLASH (dest[-1]); --dest)
                     continue;
            }
         }
         else if (! (suffix_requires_dir_check (end)
                     ? dir_check (rname, dest)
                     : errno == EINVAL))
            goto error;
      }
   }
   if (dest > rname + prefix_len + 1 && ISSLASH (dest[-1]))
      --dest;
   failed = 0;

  error:
   *dest++ = '\0';
   if (resolved != NULL)
   {
      /* Copy the full result on success or partial result if failure was due
         to the path not existing or not being accessible.  */
      if ((!failed || errno == ENOENT || errno == EACCES)
          && dest - rname <= get_path_max ())
      {
         strcpy (resolved, rname);
         if (failed)
            return NULL;
         else
            return resolved;
      }
      if (!failed)
         spindlerp_set_errno (ENAMETOOLONG);
      return NULL;
   }
   else
   {
      if (failed)
         return NULL;
      else {
         result = spindlerp_ext_strdup (bufs->rname.data);
         if (!result) {
            result = orig_realpath(name, resolved);
         }
         return result;
      }
   }
}

/* Return the canonical absolute name of file NAME.  A canonical name
   does not contain any ".", ".." components nor any repeated file name
   separators ('/') or symlinks.  All file name components must exist.  If
   RESOLVED is null, the result is malloc'd; otherwise, if the
   canonical name is PATH_MAX chars or more, returns null with 'errno'
   set to ENAMETOOLONG; if the name fits in fewer than PATH_MAX chars,
   returns the name in RESOLVED.  If the name cannot be resolved and
   RESOLVED is non-NULL, it contains the name of the first component
   that cannot be resolved.  If the name can be resolved, RESOLVED
   holds the same value as the value returned.  */

char *spindle_realpath (const char *name, char *resolved)
{
   struct realpath_bufs bufs;
   scratch_buffer_init (&bufs.rname);
   scratch_buffer_init (&bufs.extra);
   scratch_buffer_init (&bufs.link);
   char *result = realpath_stk (name, resolved, &bufs);
   scratch_buffer_free (&bufs.link);
   scratch_buffer_free (&bufs.extra);
   scratch_buffer_free (&bufs.rname);
   debug_printf3("spindle_realpath(%s, %p) returning %s\n", name, resolved, result);
   if (result)
      set_errno(0);
   return result;
}

#if defined(RPTEST)
int main(int argc, char *argv[]) {
   char *s;
   char out[4096];
   if (argc == 1) {
      return -1;
   }
   printf("stat() 1\n");
   fflush(stdout);
   s = spindle_realpath(argv[1], out);
   printf("spindle_realpath - '%s'\n", s ? out : "[NULL]");
   fflush(stdout);
   printf("stat() 2\n");
   fflush(stdout);
   s = realpath(argv[1], out);
   printf("  glibc_realpath - '%s'\n", s ? out : "[NULL]");
   return 0;
}
#endif
