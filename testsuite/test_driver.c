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

#include "config.h"
#if defined(HAVE_MPI)
#include <mpi.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>
#include <unistd.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <dirent.h>
#include <pthread.h>

#include "spindle.h"
#include "ccwarns.h"

#if !defined(LPATH)
#error LPATH must be defined
#endif

#define STR(x) STR2(x)
#define STR2(x) #x

#if defined(__cplusplus)
extern "C" {
#endif
   extern void spindle_test_log_msg(char *s);
#if defined(__cplusplus)
}
#endif

static char test_buffer[4096];
#define MAX_STR_SIZE 4096

int had_error;
#define err_printf(format, ...)                                         \
   do {                                                                 \
      snprintf(test_buffer, 4096, "Error - [%s:%u] - " format, __FILE__, __LINE__, ## __VA_ARGS__); \
      spindle_test_log_msg(test_buffer);                                \
      fprintf(stderr, "%s\n", test_buffer);                             \
      had_error = 1;                                                    \
   } while (0); 

#define test_printf(format, ...)                                        \
   do {                                                                 \
      snprintf(test_buffer, 4096, format, ## __VA_ARGS__);              \
      spindle_test_log_msg(test_buffer);                                \
   } while(0);                                                          \

typedef enum 
{
   om_unset,
   om_ldpreload,
   om_dependency,
   om_dlopen,
   om_thrdopen,
   om_dlreopen,
   om_reorder,
   om_partial,
   om_spindleapi
} open_mode_t;

open_mode_t open_mode = om_unset;

typedef struct {
   char *libname;
   void *dlhandle;
   int (*calc_func)(void);
   int (*tls_func)(void);
   int opened;
   int flags;
   char *subdir;
} open_libraries_t;

#define UNLOADED 0
#define DLOPENED 1
#define STARTUP_LOAD 2

#define FLAGS_MUSTOPEN (1 << 0)
#define FLAGS_NOEXIST  (1 << 1)
#define FLAGS_SYMLINK  (1 << 2)
#define FLAGS_SKIP     (1 << 3)
#define FLAGS_WONTLOAD (1 << 4)
#define FLAGS_TLSLIB   (1 << 5)
#define FLAGS_CHECKSYMT (1 << 6)
#define FLAGS_LOCAL (1 << 7)
#define FLAGS_OPEN_AFTER_FORK (1 << 8)

int abort_mode = 0;
int fork_mode = 0;
int fork_mode_is_child = 0;
int fork_child = 0;
int forkexec_mode = 0;
int nompi_mode = 0;
int preload_mode = 0;
int chdir_mode = 0;

int gargc;
char **gargv;

static int getUniqueHostPerNode();
static int collectResults();
static int collect_forkmode(int passed);
static void setup_forkmode();

GCC7_DISABLE_WARNING("-Wformat-truncation");

open_libraries_t libraries[] = {
   { "libtest10.so", NULL, NULL, NULL, UNLOADED, FLAGS_MUSTOPEN, NULL },
   { "libtest11.so", NULL, NULL, NULL, UNLOADED, 0, NULL },
   { "libtest12.so", NULL, NULL, NULL, UNLOADED, 0, NULL },
   { "libtest13.so", NULL, NULL, NULL, UNLOADED, 0, NULL },
   { "libtest14.so", NULL, NULL, NULL, UNLOADED, 0, NULL },
   { "libtest15.so", NULL, NULL, NULL, UNLOADED, 0, NULL },
   { "libtest16.so", NULL, NULL, NULL, UNLOADED, 0, NULL },
   { "libtest17.so", NULL, NULL, NULL, UNLOADED, 0, NULL },
   { "libtest18.so", NULL, NULL, NULL, UNLOADED, 0, NULL },
   { "libtest19.so", NULL, NULL, NULL, UNLOADED, 0, NULL },
   { "libtest20.so", NULL, NULL, NULL, UNLOADED, 0, NULL },
   { "libtest50.so", NULL, NULL, NULL, UNLOADED, 0, NULL },
   { "libtest100.so", NULL, NULL, NULL, UNLOADED, 0, NULL },
   { "libtest500.so", NULL, NULL, NULL, UNLOADED, 0, NULL },
   { "libtest1000.so", NULL, NULL, NULL, UNLOADED, 0, NULL },
   { "libtest2000.so", NULL, NULL, NULL, UNLOADED, 0, NULL },
   { "libtest4000.so", NULL, NULL, NULL, UNLOADED, 0, NULL },
   { "libtest6000.so", NULL, NULL, NULL, UNLOADED, 0, NULL },
   { "libtest8000.so", NULL, NULL, NULL, UNLOADED, 0, NULL },
   { "libtest10000.so", NULL, NULL, NULL, UNLOADED, 0, NULL },
   { "libdepA.so", NULL, NULL, NULL, UNLOADED, FLAGS_OPEN_AFTER_FORK, NULL },
   { "libcxxexceptA.so", NULL, NULL, NULL, UNLOADED, FLAGS_OPEN_AFTER_FORK, NULL },
   { "libnoexist.so", NULL, NULL, NULL, UNLOADED, FLAGS_NOEXIST, NULL },
   { "libsymlink.so", NULL, NULL, NULL, UNLOADED, FLAGS_SYMLINK | FLAGS_SKIP | FLAGS_WONTLOAD, NULL },
   { "liboriginlib.so", NULL, NULL, NULL, UNLOADED, FLAGS_OPEN_AFTER_FORK, "origin_dir/" },
   { "libtls1.so", NULL, NULL, NULL, UNLOADED, FLAGS_TLSLIB | FLAGS_SKIP, NULL },
   { "libtls2.so", NULL, NULL, NULL, UNLOADED, FLAGS_TLSLIB | FLAGS_SKIP, NULL },
   { "libtls3.so", NULL, NULL, NULL, UNLOADED, FLAGS_TLSLIB | FLAGS_SKIP, NULL },
   { "libtls4.so", NULL, NULL, NULL, UNLOADED, FLAGS_TLSLIB | FLAGS_SKIP, NULL },
   { "libtls5.so", NULL, NULL, NULL, UNLOADED, FLAGS_TLSLIB | FLAGS_SKIP, NULL },
   { "libtls6.so", NULL, NULL, NULL, UNLOADED, FLAGS_TLSLIB | FLAGS_SKIP, NULL },
   { "libtls7.so", NULL, NULL, NULL, UNLOADED, FLAGS_TLSLIB | FLAGS_SKIP, NULL },
   { "libtls8.so", NULL, NULL, NULL, UNLOADED, FLAGS_TLSLIB | FLAGS_SKIP, NULL },
   { "libtls9.so", NULL, NULL, NULL, UNLOADED, FLAGS_TLSLIB | FLAGS_SKIP, NULL },
   { "libtls10.so", NULL, NULL, NULL, UNLOADED, FLAGS_TLSLIB | FLAGS_SKIP, NULL },
   { "libtls11.so", NULL, NULL, NULL, UNLOADED, FLAGS_TLSLIB | FLAGS_SKIP, NULL },
   { "libtls12.so", NULL, NULL, NULL, UNLOADED, FLAGS_TLSLIB | FLAGS_SKIP, NULL },
   { "libtls13.so", NULL, NULL, NULL, UNLOADED, FLAGS_TLSLIB | FLAGS_SKIP, NULL },
   { "libtls14.so", NULL, NULL, NULL, UNLOADED, FLAGS_TLSLIB | FLAGS_SKIP, NULL },
   { "libtls15.so", NULL, NULL, NULL, UNLOADED, FLAGS_TLSLIB | FLAGS_SKIP, NULL },
   { "libtls16.so", NULL, NULL, NULL, UNLOADED, FLAGS_TLSLIB | FLAGS_SKIP, NULL },
   { "libtls17.so", NULL, NULL, NULL, UNLOADED, FLAGS_TLSLIB | FLAGS_SKIP, NULL },
   { "libtls18.so", NULL, NULL, NULL, UNLOADED, FLAGS_TLSLIB | FLAGS_SKIP, NULL },
   { "libtls19.so", NULL, NULL, NULL, UNLOADED, FLAGS_TLSLIB | FLAGS_SKIP, NULL },
   { "libtls20.so", NULL, NULL, NULL, UNLOADED, FLAGS_TLSLIB | FLAGS_SKIP, NULL },
   { "libtest10.so", NULL, NULL, NULL, UNLOADED, FLAGS_CHECKSYMT | FLAGS_SKIP, NULL },
   { "libtest11.so", NULL, NULL, NULL, UNLOADED, FLAGS_CHECKSYMT | FLAGS_SKIP, NULL },
   { "libtest12.so", NULL, NULL, NULL, UNLOADED, FLAGS_CHECKSYMT | FLAGS_SKIP, NULL },
   { "libtest13.so", NULL, NULL, NULL, UNLOADED, FLAGS_CHECKSYMT | FLAGS_SKIP, NULL },
   { "libtest14.so", NULL, NULL, NULL, UNLOADED, FLAGS_CHECKSYMT | FLAGS_SKIP, NULL },
   { "libtest15.so", NULL, NULL, NULL, UNLOADED, FLAGS_CHECKSYMT | FLAGS_SKIP, NULL },
   { "libtest16.so", NULL, NULL, NULL, UNLOADED, FLAGS_CHECKSYMT | FLAGS_SKIP, NULL },
   { "libtest17.so", NULL, NULL, NULL, UNLOADED, FLAGS_CHECKSYMT | FLAGS_SKIP, NULL },
   { "libtest18.so", NULL, NULL, NULL, UNLOADED, FLAGS_CHECKSYMT | FLAGS_SKIP, NULL },
   { "libtest19.so", NULL, NULL, NULL, UNLOADED, FLAGS_CHECKSYMT | FLAGS_SKIP, NULL },
   { "libtest20.so", NULL, NULL, NULL, UNLOADED, FLAGS_CHECKSYMT | FLAGS_SKIP, NULL },
   { "libtest50.so", NULL, NULL, NULL, UNLOADED, FLAGS_CHECKSYMT | FLAGS_SKIP, NULL },
   { "libtest100.so", NULL, NULL, NULL, UNLOADED, FLAGS_CHECKSYMT | FLAGS_SKIP, NULL },
   { "libtest500.so", NULL, NULL, NULL, UNLOADED, FLAGS_CHECKSYMT | FLAGS_SKIP, NULL },
   { "libtest1000.so", NULL, NULL, NULL, UNLOADED, FLAGS_CHECKSYMT | FLAGS_SKIP, NULL },
   { "libtest2000.so", NULL, NULL, NULL, UNLOADED, FLAGS_CHECKSYMT | FLAGS_SKIP, NULL },
   { "libtest4000.so", NULL, NULL, NULL, UNLOADED, FLAGS_CHECKSYMT | FLAGS_SKIP, NULL },
   { "libtest6000.so", NULL, NULL, NULL, UNLOADED, FLAGS_CHECKSYMT | FLAGS_SKIP, NULL },
   { "libtest8000.so", NULL, NULL, NULL, UNLOADED, FLAGS_CHECKSYMT | FLAGS_SKIP, NULL },
   { "libtest10000.so", NULL, NULL, NULL, UNLOADED, FLAGS_CHECKSYMT | FLAGS_SKIP, NULL },
   { "liblocal.so", NULL, NULL, NULL, UNLOADED, FLAGS_LOCAL, NULL },
   { "libexeorigin.so", NULL, NULL, NULL, UNLOADED, FLAGS_OPEN_AFTER_FORK, "exeorigin_dir/" },
   { NULL, NULL, NULL, 0, 0 }
};
int num_libraries;

#define DEPENDENCY_HANDLE ((void *) 1)

void set_lib_functions(int (*calcfunc)(void), int(*tlsfunc)(void), char *name)
{
   int i;
   for (i = 0; libraries[i].libname; i++) {
      if (strcmp(libraries[i].libname, name) != 0)
         continue;
      libraries[i].calc_func = calcfunc;
      libraries[i].tls_func = tlsfunc;
      return;
   }
   err_printf("Failed to find function %s in list\n", name);
}

typedef int (*func_t)(void);
typedef void (*cb_func_t)(func_t, func_t, char *);
extern void setup_func_callback(cb_func_t);

static char oldcwd[4096];

char *libpath(char *s, char *subdir) {
   static char path[4096];
   if (!subdir)
      subdir = "";
   if (chdir_mode)
      snprintf(path, 4096, "%s/%s%s", oldcwd, subdir, s);
   else
      snprintf(path, 4096, "%s/%s%s", STR(LPATH), subdir, s);
   return path;
}

static void check_symt(char *path)
{
   struct stat buf;
   int fd = -1, result;
   void *mmap_result = NULL;
   unsigned char *base;
   ElfW(Ehdr) *elf_header;
   ElfW(Off) section_offset, symt_offset = 0;
   ElfW(Half) section_cnt;
   ElfW(Shdr) *section_table, *section_table_end, *sec;
   ElfW(Xword) symt_size;
   ElfW(Sym) *symtable, *symtable_end, *sym;
   int found_non_zero_symbol = 0;

   fd = open(path, O_RDONLY);
   if (fd == -1) {
      int error = errno;
      err_printf("Could not open(%s, O_RDONLY) for symt test: %s\n", path, strerror(error));
      goto done;
   }
   result = fstat(fd, &buf);
   if (result == -1) {
      int error = errno;
      err_printf("Could not fstat(%s (%d)): %s\n", path, fd, strerror(error));
      goto done;
   }

   mmap_result = mmap(NULL, buf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
   if (mmap_result == MAP_FAILED) {
      int error = errno;
      err_printf("Could not mmap %s of size %lu: %s\n", path, buf.st_size, strerror(error));
      goto done;
   }
   base = (unsigned char *) mmap_result;

   if (sizeof(ElfW(Ehdr)) >=  buf.st_size) {
      err_printf("File size %lu is less than size of elf header %lu\n", buf.st_size, sizeof(ElfW(Ehdr)));
      goto done;
   }
   elf_header = (ElfW(Ehdr) *) mmap_result;
   section_offset = elf_header->e_shoff;
   section_cnt = elf_header->e_shnum;

   if (section_offset + (section_cnt*sizeof(ElfW(Shdr))) > buf.st_size) {
      err_printf("File size %lu is less than section table %lu\n", buf.st_size, (unsigned long) (section_offset + ((sizeof(ElfW(Shdr))*section_cnt))));
      goto done;
   }
   section_table = (ElfW(Shdr) *) (base + section_offset);
   section_table_end = section_table + section_cnt;
   for (sec = section_table; sec != section_table_end; sec++) {
      if (sec->sh_type == SHT_SYMTAB && !sec->sh_addr) {
         symt_offset = sec->sh_offset;
         symt_size = sec->sh_size;
         break;
      }
   }
   if (!symt_offset) {
      err_printf("Could not find symbol table in %s\n", path);
      goto done;
   }
   if (symt_offset + symt_size > buf.st_size) {
      err_printf("File size %lu is less than size of symbol table %lu\n", buf.st_size, (unsigned long) (symt_offset + symt_size));
      goto done;
   }
   
   symtable = (ElfW(Sym) *) (base + symt_offset);
   symtable_end = (ElfW(Sym) *) (base + symt_offset + symt_size);
   for (sym = symtable; sym != symtable_end; sym++) {
      if (sym->st_name) {
         found_non_zero_symbol = 1;
         break;
      }
   }
   if (!found_non_zero_symbol) {
      err_printf("Could not find symbol in %s that was not zeros\n", path);
      goto done;
   }

  done:
   if (mmap_result)
      munmap(mmap_result, buf.st_size);
   /**
    * Don't close fds from open. We'll test them later to see if readlink sees spindle paths in them
    *    if (fd != -1)
    *       close(fd);
    */
}

static char *get_local_name(int libnum, char *out, int out_size)
{
   char *tmpdir;
   char tmp[MAX_STR_SIZE];

   tmpdir = getenv("TMPDIR");
   if (!tmpdir)
      tmpdir = getenv("TMP");
   if (!tmpdir)
      tmpdir = "/tmp";

   realpath(tmpdir, tmp);

   snprintf(out, out_size, "%s/%s", tmp, libraries[libnum].libname);
   out[out_size-1] = '\0';
   return out;
}

static void clean_local_libs()
{
   char out_filename[MAX_STR_SIZE];
   int i;

   for (i = 0; libraries[i].libname; i++) {
      get_local_name(i, out_filename, sizeof(out_filename));
      unlink(out_filename);
   }
}

static char *create_local_file(int libnum)
{
   static char out_filename[MAX_STR_SIZE];
   char hostname[256];
   char *in_filename, *result_filename = NULL;
   char *contents = MAP_FAILED;
   int in_fd = -1, out_fd = -1;
   int result, i, found = 0;
   size_t in_size;
   struct stat statbuf;
   int timeout = 50;
   const char *key = "SPINDLE_PLACEHOLDER";

   memset(hostname, 0, sizeof(hostname));
   gethostname(hostname, sizeof(hostname)-1);

   in_filename = libpath(libraries[libnum].libname, libraries[libnum].subdir);
   in_fd = open(in_filename, O_RDONLY);
   if (in_fd == -1) {
      err_printf("Failed to open %s\n", in_filename);
      goto done;
   }
   result = fstat(in_fd, &statbuf);
   if (result == -1) {
      err_printf("Failed to stat %s\n", in_filename);
      goto done;
   }
   in_size = statbuf.st_size;

   contents = (char *) mmap(NULL, in_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, in_fd, 0);
   if (contents == MAP_FAILED) {
      err_printf("Failed to mmap %s\n", in_filename);
      goto done;
   }

   for (i = 0; i < in_size - strlen(key); i++) {
      if (contents[i] != key[0])
         continue;
      if (contents[i+1] != key[1])
         continue;
      if (contents[i+2] != key[2])
         continue;
      if (strncmp(contents+i, key, strlen(key)) == 0) {
         strncpy(contents+i, hostname, sizeof(hostname)-1);
         found = 1;
         break;
      }
   }
   if (!found) {
      err_printf("Failed to find placeholder in %s\n", in_filename);
      goto done;
   }

   get_local_name(libnum, out_filename, sizeof(out_filename));
   out_fd = open(out_filename, O_WRONLY | O_CREAT | O_EXCL, S_IRWXU);
   if (out_fd == -1 && errno != EEXIST) {
      err_printf("Failed to open output file %s\n", out_filename);
      goto done;
   }
   if (out_fd != -1) {
      i = 0;
      do {
         result = write(out_fd, contents+i, in_size-i);
         if (result == -1 && errno == EINTR) {
            continue;
         }
         else if (result == -1) {
            err_printf("Failed to write to %s", out_filename);
            goto done;
         }
         i += result;
      } while (i < in_size);
      close(out_fd);
      out_fd = -1;
   }

   while (timeout > 0) {
      int result = stat(out_filename, &statbuf);
      if (result == 0 && statbuf.st_size == in_size)
         break;
      usleep(1000);
      timeout--;
   }
   if (!timeout) {
      err_printf("Timed out waiting for %s to be created and reach proper size\n", out_filename);
      goto done;
   }

   result_filename = out_filename;

  done:
   if (contents != MAP_FAILED)
      munmap(contents, in_size);
   if (in_fd != -1)
      close(in_fd);
   if (out_fd != -1)
      close(out_fd);
   return result_filename;
}


static void open_library(int i)
{
   char *fullpath;
   void *result;
   if (libraries[i].flags & FLAGS_TLSLIB)
      return;
   else if (!(libraries[i].flags & FLAGS_LOCAL))
      fullpath = libpath(libraries[i].libname, libraries[i].subdir);
   else
      fullpath = create_local_file(i);
   
   if (libraries[i].flags & FLAGS_CHECKSYMT) {
      check_symt(fullpath);
      return;
   }   
   if (!(libraries[i].flags & FLAGS_WONTLOAD) && !(libraries[i].flags & FLAGS_LOCAL)) {
      test_printf("dlstart %s\n", libraries[i].libname);
   }

   result = dlopen(fullpath, RTLD_LAZY | RTLD_GLOBAL);
   if (libraries[i].flags & FLAGS_NOEXIST) {
      if (result)
         err_printf("Failure, opened a library that doesn't exist\n");
      return;
   }
   if (libraries[i].flags & FLAGS_SKIP) {
      return;
   }
   libraries[i].opened = DLOPENED;
   if (!result) {
      err_printf("Failed to dlopen library %s: %s\n", fullpath, dlerror());
      return;
   }
   libraries[i].dlhandle = result;
}

static int hostname_seed()
{
   int seed = 0, i;
   char hostname[1024];
   gethostname(hostname, 1024);
   for (i=0; hostname[i] != '\0'; i++) {
      seed += hostname[i];
   }
   return seed;
}

void dependency_mode()
{
   /* Should be auto loaded */
   int i;
   for (i = 0; i<num_libraries; i++) {
      if (fork_mode_is_child && libraries[i].flags & FLAGS_OPEN_AFTER_FORK) {
         char *fullpath = libpath(libraries[i].libname, libraries[i].subdir);
         int fd = open(fullpath, O_RDONLY);
         if (fd != -1) close(fd);
      }
      if (libraries[i].flags & FLAGS_CHECKSYMT)
         open_library(i);
      if (libraries[i].flags & FLAGS_LOCAL)
         open_library(i);
      if (libraries[i].flags & FLAGS_NOEXIST || libraries[i].flags & FLAGS_SKIP || libraries[i].flags & FLAGS_WONTLOAD || libraries[i].flags & FLAGS_LOCAL)
         continue;
      libraries[i].opened = STARTUP_LOAD;
      test_printf("dlstart %s\n", libraries[i].libname);
   }
}

void ldpreload_mode()
{
   unsigned i;
   char *env = getenv("LD_PRELOAD");
   if (!env) {
      err_printf("LD_PRELOAD Unset\n");
      return;
   }

   dependency_mode();
   for (i = 0; i < num_libraries; i++) {
      if (libraries[i].flags & FLAGS_NOEXIST || libraries[i].flags & FLAGS_SKIP || libraries[i].flags & FLAGS_LOCAL)
         continue;
      if (strstr(env, libraries[i].libname) == NULL) {
         err_printf("Could not find library %s in LD_PRELOAD (%s)\n", libraries[i].libname, env);
         return;
      }
   }
}

void dlopen_mode()
{
   int i;
   for (i = 0; i<num_libraries; i++) {
      open_library(i);
   }
}

static void* thrd_dlopen_mode(void *arg)
{
   dlopen_mode();
   return NULL;
}

void thrd_mode()
{
   pthread_t thrd;
   int result;

   result = pthread_create(&thrd, NULL, thrd_dlopen_mode, NULL);
   if (result != 0) {
      err_printf("pthread_create returned error code %d\n", result);
      return;
   }
   pthread_join(thrd, NULL);
}

void dlreopen_mode()
{
   dependency_mode();
   dlopen_mode();
}

void reorder_mode()
{
   /* Load libraries in random (based on hostname) order */
   int i, initial;
   i = initial = hostname_seed() % num_libraries;
   do {
      open_library(i);
      i++;
      if (i == num_libraries) {
         i = 0;
      }
   } while (i != initial);
}

void partial_mode()
{
   /* Load some libraries in random (based on hostname) order,
      randomly skip approximately half the libraries. */
   int i, initial;
   int seed = hostname_seed();
   srand(seed);
   i = initial = (seed % num_libraries);
   do {
      if ((rand() & 1) || (libraries[i].flags & FLAGS_MUSTOPEN)) {
         open_library(i);
      }
      i++;
      if (i == num_libraries) {
         i = 0;
      }
   } while (i != initial);   
}

void api_mode()
{
   int i;
   errno = 0;
   for (i = 0; libraries[i].libname; i++) {
      int result;
      struct stat buf1, buf2;
      char *path;
      path = libpath(libraries[i].libname, libraries[i].subdir);
      
      result = spindle_stat(path, &buf1);
      if (libraries[i].flags & FLAGS_NOEXIST) {
         if (result != -1 || errno != ENOENT) {
            err_printf("Bad error return from spindle_stat\n");
         }
         memset(&buf1, 0, sizeof(buf1));
         result = 0;
      }
      if (result == -1) {
         err_printf("Failed to spindle_stat file %s\n", path);
      }

      result = stat(path, &buf2);
      if (libraries[i].flags & FLAGS_NOEXIST) {
         if (result != -1 || errno != ENOENT) {
            err_printf("Bad error return from stat\n");
         }
         memset(&buf2, 0, sizeof(buf2));
         result = 0;
      }
      if (result == -1) {
         err_printf("Failed to stat file %s\n", path);
      }

      if (buf1.st_size != buf2.st_size) {
         err_printf("Failed, stats gave different sizes on %s\n", path);
      }

      if (libraries[i].flags & FLAGS_SYMLINK) {
         struct stat lbuf1, lbuf2;
         result = spindle_lstat(path, &lbuf1);
         if (result == -1) {
            err_printf("Failed to spindle_lstat file %s\n", path);
         }
         result = lstat(path, &lbuf2);
         if (result == -1) {
            err_printf("Failed to lstat file %s\n", path);
         }
         if (lbuf1.st_size != lbuf2.st_size) {
            err_printf("Failed, lstats gave different sizes on %s\n", path);
         }
         if (!S_ISLNK(lbuf1.st_mode)) {
            err_printf("Failed, spindle_lstat wasn't to symbolic link\n");
         }
         if (!S_ISLNK(lbuf2.st_mode)) {
            err_printf("Failed, lstat wasn't to symbolic link\n");
         }
      }

      uint32_t sig = 0;

      if (i % 2 == 0) {
         int fd = spindle_open(path, O_RDONLY);
         if (libraries[i].flags & FLAGS_NOEXIST) {
            if (fd != -1 || errno != ENOENT) {
               err_printf("Bad error return from spindle_stat\n");
            }
            continue;
         }
         if (fd == -1) {
            err_printf("Failed to open %s\n", path);
         }
         else {
            int result = read(fd, &sig, sizeof(sig));
            if (result == -1) {
               err_printf("Failed to read header from %s\n", path);
            }
            close(fd);
         }
      }
      else {
         FILE *fd = spindle_fopen(path, "r");
         if (libraries[i].flags & FLAGS_NOEXIST) {
            if (fd != NULL || errno != ENOENT) {
               err_printf("Bad error return from spindle_stat\n");
            }
            continue;
         }
         if (fd == NULL) {
            err_printf("Failed to fopen %s\n", path);
         }
         else {
            int result = fread(&sig, sizeof(sig), 1, fd);
            if (result != 1) {
               err_printf("Failed to fread from %s\n", path);
            }
            fclose(fd);
         }
      }

      if (sig != 0x7F454C46 && sig != 0x464c457f) {
         err_printf("Read file header %x, which wasn't elf header from %s\n", sig, path);
      }
   }
}

void open_libraries()
{
   switch (open_mode) {
      case om_unset:
         err_printf("Open mode was not set\n");
         break;
      case om_ldpreload:
         ldpreload_mode();
         break;
      case om_dependency:
         dependency_mode();
         break;
      case om_dlopen:
         dlopen_mode();
         break;
      case om_thrdopen:
         thrd_mode();
         break;
      case om_dlreopen:
         dlreopen_mode();
         break;
      case om_reorder:
         reorder_mode();
         break;
      case om_partial:
         partial_mode();
         break;
      case om_spindleapi:
         api_mode();
         break;
   }      
}

static int run_exec_test(const char *prefix, const char *path, int expected)
{
   int pathsearch;
   char newpath[4097];
   newpath[4096] = '\0';
   if (prefix) {
      snprintf(newpath, 4097, "%s/%s", prefix, path);
      pathsearch = 0;
   }
   else {
      snprintf(newpath, 4097, "%s", path);
      pathsearch = 1;
   }

   int pid = fork();
   if (pid == -1) {
      err_printf("%s could not fork\n", newpath);
      return -1;
   }
   if (pid == 0) {
      if (expected == 0) {
         close(-1); /* A close call does a check_for_fork under the hood, re-initing the
                       logger connection for the following print */
         test_printf("dlstart %s\n", path);
      }
      
      char* args[2];
      args[0] = newpath;
      args[1] = NULL;
      if (pathsearch)
         execvp(newpath, args);
      else
         execv(newpath, args);
      _exit(errno);
   }

   int status, result;
   do {
      result = waitpid(pid, &status, 0);
      if (WIFSIGNALED(status)) {
         err_printf("%s unexpectedly exited on signal %d\n", newpath, WTERMSIG(status));
         return -1;
      }
      if (result == -1) {
         err_printf("%s had unexpected waitpid failure\n", newpath);
         return -1;
      }
   } while (result != pid && !WIFEXITED(status));
   if (expected != WEXITSTATUS(status)) {
      err_printf("%s exited with return code %d, expected %d\n", newpath, WEXITSTATUS(status), expected);
      return -1;
   }
   return 0;
}

static int run_exec_sets(const char *prefix)
{
   int result = 0;

   result |= run_exec_test(prefix, "retzero_rx", 0);
   result |= run_exec_test(prefix, "retzero_x", 0);
   result |= run_exec_test(prefix, "retzero_r", EACCES);
   result |= run_exec_test(prefix, "retzero_", EACCES);   
   result |= run_exec_test(prefix, "nofile", ENOENT);
   result |= run_exec_test(prefix, "nodir/nofile", ENOENT);
   result |= run_exec_test(prefix, "..", EACCES);
   //result |= run_exec_test(prefix, "badinterp", ENOENT);
   return result;
}

static int run_execs()
{
   if (fork_mode || forkexec_mode || nompi_mode || chdir_mode)
      return 0;
   
   int result = run_exec_sets(STR(LPATH));
   if (result == -1)
      return -1;
      
   //Add LPATH to PATH environment variable and run again without a prefix
   char *path = getenv("PATH");
   int len = strlen(path) + strlen(STR(LPATH)) + 2;
   char *newpath = (char *) malloc(len);
   snprintf(newpath, len, "%s:%s", path, STR(LPATH));
   return run_exec_sets(NULL);
}


#define STAT 1
#define LSTAT 2
#define FSTAT 4
#define SPINDLE_STAT 8
static dev_t device;
static int run_stat_test(const char *file, int flags, mode_t prot, int expected)
{
   struct stat buf;
   int result = -1;
   int fd = -1;
   const char *statname = NULL;

   if (expected == 0) {
      test_printf("dlstart %s\n", file[0] != '/' ? file : file+1);
   }
   if (flags & LSTAT) {
      statname = "lstat";
      result = lstat(file, &buf);
   }
   else if (flags & FSTAT) {
      statname = "fstat";      
      fd = open(file, O_RDONLY);
      if (fd != -1)
         result = fstat(fd, &buf);
   }
   else if (flags & STAT) {
      statname = "stat";      
      result = stat(file, &buf);
   }
   else if (flags & SPINDLE_STAT) {
      statname = "spindle_stat";
      result = spindle_stat(file, &buf);
   }
   if (result == -1)
      result = errno;
   if (fd != -1)
      close(fd);
   
   if (result != expected) {
      err_printf("Expected return value %d, got return value %d (%s) from %s test of %s\n",
                 expected, result, strerror(result), statname, file);
      return -1;
   }
   if (result)
      //Expected error return, do not test buf
      return 0;
   
   if (buf.st_dev != device && !(flags & SPINDLE_STAT)) {
      err_printf("Expected device %d, got device %d on %s test of %s\n",
                 (int) device, (int) buf.st_dev, statname, file);
      return -1;
   }
   if (prot && ((buf.st_mode & 0700) != prot)) {
      err_printf("Expected prot %o, got prot %o on %s test of %s\n",
                 prot, buf.st_mode & 0700, statname, file);
      return -1;      
   }
   
   return 0;
}

static int set_device()
{
   struct stat buf;
   int result;
   
   result = stat("retzero_rx", &buf);
   if (result == -1) {
      err_printf("Could not get device of retzero_rx");
      return -1;
   }
   device = buf.st_dev;
   return 0;
}

static int run_stats()
{
   int result;

   if (fork_mode || forkexec_mode || nompi_mode || chdir_mode)
      return 0;
   
   result = set_device();

   result |= run_stat_test("hello_r.py", STAT, 0600, 0);
   result |= run_stat_test("hello_x.py", STAT, 0300, 0);
   result |= run_stat_test("hello_rx.py", STAT, 0700, 0);
   result |= run_stat_test("hello_.py", STAT, 0200, 0);
   result |= run_stat_test("hello_l.py", STAT, 0300, 0);
   result |= run_stat_test("noexist.py", STAT, 0000, ENOENT);
   result |= run_stat_test("/nodir/nofile.py", STAT, 0000, ENOENT);
   /* result |= run_stat_test("retzero_/nofile.py", STAT, 0000, ENOTDIR); */
   result |= run_stat_test("badlink.py", STAT, 0000, ENOENT);
   result |= run_stat_test(".", STAT, 0000, 0);
   result |= run_stat_test(NULL, STAT, 0000, EFAULT);

   result |= run_stat_test("hello_r.py", FSTAT, 0600, 0);
   result |= run_stat_test("hello_x.py", FSTAT, 0000, EACCES);
   result |= run_stat_test("hello_rx.py", FSTAT, 0700, 0);
   result |= run_stat_test("hello_.py", FSTAT, 0000, EACCES);
   result |= run_stat_test("hello_l.py", FSTAT, 0000, EACCES);
   result |= run_stat_test("noexist.py", FSTAT, 0000, ENOENT);
   result |= run_stat_test("/nodir/nofile.py", FSTAT, 0000, ENOENT);
   /* result |= run_stat_test("retzero_/nofile.py", FSTAT, 0000, ENOTDIR); */
   result |= run_stat_test("badlink.py", FSTAT, 0000, ENOENT);
   result |= run_stat_test(".", FSTAT, 0000, 0);
   result |= run_stat_test(NULL, FSTAT, 0000, EFAULT);

   result |= run_stat_test("hello_r.py", LSTAT, 0600, 0);
   result |= run_stat_test("hello_x.py", LSTAT, 0300, 0);
   result |= run_stat_test("hello_rx.py", LSTAT, 0700, 0);
   result |= run_stat_test("hello_.py", LSTAT, 0200, 0);
   result |= run_stat_test("hello_l.py", LSTAT, 0700, 0);
   result |= run_stat_test("noexist.py", LSTAT, 0000, ENOENT);
   result |= run_stat_test("/nodir/nofile.py", LSTAT, 0000, ENOENT);
   /* result |= run_stat_test("retzero_/nofile.py", LSTAT, 0000, ENOTDIR); */
   result |= run_stat_test("badlink.py", LSTAT, 0700, 0);
   result |= run_stat_test(".", LSTAT, 0000, 0);
   result |= run_stat_test(NULL, LSTAT, 0000, EFAULT);

   result |= run_stat_test("/bin", SPINDLE_STAT, 0000, 0);
   
   return result;
}

static int run_readlink_test(char *path, int buflen, char *expected, int expected_err)
{
   char buf[4096];
   ssize_t result;
   int error, i;

   if (chdir_mode)
      return 0;
   
   memset(buf, 'X', sizeof(buf));
   errno = 0;
   result = readlink(path, buf, buflen ? buflen : sizeof(buf));
   error = errno;
   if (expected) {
      for (i = 0; i < sizeof(buf); i++) {
         if (buf[i] == 'X') {
            buf[i] = '\0';
            break;
         }
      }
      if (strncmp(buf, expected, buflen ? buflen : sizeof(buf)) != 0) {
         err_printf("readlink(%s) returned unexpected string '%s'. Expected %s\n",
                    path, buf[0] == 'X' ? "[XSTRING]" : buf, expected);
         had_error = 1;
      }
      if (result != strlen(expected)) {
         err_printf("readlink(%s) -> '%s' returned result %ld. Expected %lu\n",
                    path, buf[0] == 'X' ? "[XSTRING]" : buf, result, strlen(expected));
         had_error = 1;
      }
      if (buflen && buf[buflen+1] != 'X') {
         err_printf("readlink(%s) -> '%s' overwrote character at %d with %d (%c)\n",
                    path, buf[0] == 'X' ? "[XSTRING]" : buf, buflen,
                    (int) buf[buflen], buf[buflen]);
         had_error = 1;
      }
      if (error) {
         err_printf("readlink(%s) set errno to %d when it should return success\n",
                    path, error);
         had_error = 1;
      }
   }
   else if (expected_err) {
      if (result != -1) {
         err_printf("readlink(%s) returned %ld instead of -1\n", path, result);
         had_error = 1;
      }
      if (buf[0] != 'X') {
         err_printf("readlink(%s) should have returned error, but overwrote output string with %s\n",
                    path, buf);
         had_error = 1;
      }
      if (error != expected_err) {
         err_printf("readlink(%s) expected error %d. Got error %d\n",
                    path, expected_err, error);
         had_error = 1;
      }
   }
   else {
      assert(0);
   }
   return had_error;
}

static int run_readlinks()
{
   int result = 0;

   result |= run_readlink_test("badlink.py", 0, "noexist.py", 0);
   result |= run_readlink_test("hello_l.py", 0, "hello_x.py", 0);
   result |= run_readlink_test("libsymlink.so", 0, "libtest10.so", 0);
   result |= run_readlink_test("hello_l.py", 5, "hello", 0);
   result |= run_readlink_test("nolink.py", 0, NULL, ENOENT);
   result |= run_readlink_test("hello_.py", 0, NULL, EINVAL);
   result |= run_readlink_test("hello_r.py", 0, NULL, EINVAL);
   result |= run_readlink_test("origin_dir", 0, NULL, EINVAL);
   result |= run_readlink_test("nodir/file.py", 0, NULL, ENOENT);

   return result;
}

static int run_alias_test(char *path, int expected_err) {
   int fd, result, error;
   char buffer[5];
   fd = open(path, O_RDONLY);
   if (fd == -1) {
      error = errno;
      if (!expected_err) {
         err_printf("alias_test %s: open expected success, but got error %s (%d)\n", path, strerror(error), error);
         return -1;
      }
      else if (error != expected_err) {
         err_printf("alias_test %s: open returned wrong error. Expected %s (%d), got %s (%d)\n",
                    path, strerror(expected_err), expected_err, strerror(error), error);
         return -1;
      }
      else {
         return 0;
      }
   }
   if (fd >= 0 && expected_err) {
      err_printf("alias_test %s: open expected error %s (%d), but returned succcess\n", path, strerror(expected_err), expected_err);
      close(fd);
      return -1;
   }

   result = read(fd, buffer, 4);
   if (result == -1) {
      error = errno;
      err_printf("alias_test %s: error reading from buffer: %s (%d)\n", path, strerror(error), error);
      close(fd);
      return -1;
   }

   buffer[4] = '\0';
   if (strcmp(buffer, "pass") != 0) {
      err_printf("alias_test %s: Did not read correct file. Got string '%s' from file\n", path, buffer);
      close(fd);
      return -1;
   }

   close(fd);
   return 0;
}

int run_alias_tests()
{
   int result = 0, i;
   int slashes_in_cwd = 0;
   char path[4096];
   char cwd[4096];
   char slashes[4096];

   if (chdir_mode)
      return 0;
   
   slashes[0] = '\0';
   getcwd(cwd, 4096);

   for (i = 0; cwd[i] != '\0'; i++) 
      if (cwd[i] == '/') slashes_in_cwd++;
     
   for (i = 0; i < slashes_in_cwd + 2; i++) {
      strncat(slashes, "../", 4096 - (i * 3));
   }
      
   result |= run_alias_test("alias/lnk/../aliastest.py", 0);
   result |= run_alias_test("./alias/real1/aliastest.py", 0);
   result |= run_alias_test("./alias/real1/real2/../aliastest.py", 0);   
   result |= run_alias_test("./alias////./lnk//../////./aliastest.py", 0);
   result |= run_alias_test("alias/lnk/../real2/../aliastest.py", 0);

   snprintf(path, 4096, "%s/alias/real1/aliastest.py", cwd);
   result |= run_alias_test(path, 0);

   snprintf(path, 4096, "/%s/alias/real1/aliastest.py", cwd);
   result |= run_alias_test(path, 0);

   snprintf(path, 4096, "%s/%s/alias/real1/aliastest.py", slashes, cwd);
   result |= run_alias_test(path, 0);
   
   return result;
}

void push_cwd()
{
   getcwd(oldcwd, 4096);
   chdir("..");
}

void pop_cwd()
{
   chdir(oldcwd);
}

#define TEST_ARG(X) if (strcmp(argv[i], "--" STR(X)) == 0) open_mode = om_ ## X
#define MODE_ARG(X) if (strcmp(argv[i], "--" STR(X)) == 0) X ## _mode = 1;
void parse_args(int argc, char *argv[])
{
   if (strstr(argv[0], "test_driver") == NULL) {
      err_printf("Did not find test_driver on command line\n");
   }
   int i;
   for (i = 0; i < argc; i++) {
      TEST_ARG(ldpreload);
      TEST_ARG(dependency);
      TEST_ARG(dlopen);
      TEST_ARG(thrdopen);
      TEST_ARG(dlreopen);
      TEST_ARG(reorder);
      TEST_ARG(partial);
      TEST_ARG(spindleapi);
      MODE_ARG(abort);
      MODE_ARG(fork);
      MODE_ARG(forkexec);
      MODE_ARG(nompi);
      MODE_ARG(preload);
      MODE_ARG(chdir);
   }
   gargc = argc;
   gargv = argv;

   if (forkexec_mode)
      fork_mode = 1;
}

void call_funcs()
{
   int i, result;
   for (i = 0; i<num_libraries; i++) {
      if (libraries[i].opened == UNLOADED)
         continue;
      result = libraries[i].calc_func();
      if (result == 0) {
         err_printf("Unexpected return result of 0 from library %s\n", libraries[i].libname);
      }
   }
}

extern int get_liblist_error();

void check_libraries()
{
   int i;
   if (open_mode == om_spindleapi)
      return;

   for (i=0; i<num_libraries; i++) {
      if ((open_mode != om_partial || (libraries[i].flags & FLAGS_MUSTOPEN)) && 
          (!(libraries[i].flags & FLAGS_NOEXIST)) && 
          (!(libraries[i].flags & FLAGS_SKIP)) &&
          (!libraries[i].calc_func)) {
         err_printf("Didn't calc open expected library %s with %p\n", libraries[i].libname, libraries[i].calc_func); 
      }
      if (libraries[i].opened == UNLOADED)
         continue;
      if (libraries[i].opened == DLOPENED && !libraries[i].dlhandle)
         err_printf("Failed to dlopen library %s\n", libraries[i].libname);
      if (!libraries[i].calc_func) {
         err_printf("Failed to run library constructor in %s\n", libraries[i].libname);
      }
   }

   if (get_liblist_error()) {
      err_printf("Loaded some libraries multiple times\n");
   }
}

void checkTlsSum()
{
   int i;
   int sum = 0;

   if (open_mode == om_spindleapi)
      return;
   if (open_mode == om_thrdopen)
      return;

   for (i = 0; libraries[i].libname; i++) {
      if (!libraries[i].tls_func)
         continue;
      sum += libraries[i].tls_func();
   }
   int correct = 31815; //Sum of all the libtest*.so libraries's values
   if ((sum != correct)  && (open_mode != om_partial)) { 
      err_printf("Sum %d of TLS variables in all libtest libraries did not add to %d\n", sum, correct);
      return;
   }

   //dlopen all the libtls*.so
   sum = 0;
   for (i = 0; libraries[i].libname; i++) {
      if (!(libraries[i].flags & FLAGS_TLSLIB))
         continue;
      test_printf("dlstart %s\n", libraries[i].libname);
      void *result = dlopen(libraries[i].libname, RTLD_LAZY | RTLD_GLOBAL);
      if (!result) {
         err_printf("Failed to open library %s\n", libraries[i].libname);
         continue;
      }
      libraries[i].opened = DLOPENED;
      libraries[i].dlhandle = result;
      
      sum += libraries[i].tls_func();
   }
   correct = 210; //Sum of all the libtls*.so values
   if (sum != correct) {
      err_printf("Sum %d of TLS variables in all libtest libraries did not add to %d\n", sum, correct);
      return;
   }

}

static char* getCacheLocation(char *env_var)
{
   char *result, *last_slash;

   result = getenv(env_var);
   if (!result || result[0] == '\0')
      return NULL;

   last_slash = strrchr(result, '/');
   if (!last_slash)
      return strdup(result);
   if (last_slash[1] != '\0')
      return strdup(last_slash+1);
   while (last_slash != result && last_slash[-1] != '/') last_slash--;
   return strdup(last_slash);
}

static int checkLinkForLeak(const char *path, const char *cachepath)
{
   char link_target[4096];
   int result, error;
   memset(link_target, 0, sizeof(link_target));

   result = readlink(path, link_target, sizeof(link_target));
   if (result == -1) {
      error = errno;
      err_printf("Failed to read link %s: %s\n", path, strerror(error));
      return -1;
   }

   if (strstr(link_target, cachepath)) {
      err_printf("Link at '%s' has path '%s', which leaks spindle path with '%s'\n", path, link_target, cachepath);
      return -1;
   }

   return 0;
}


static int checkPathForLeak(const char *what, const char *path, const char *cachepath)
{
   if (strstr(path, cachepath)) {
      err_printf("%s: Path '%s' leaks spindle path with '%s'\n", what, path, cachepath);
      return -1;
   }
   return 0;
}

static int leak_check_cb(struct dl_phdr_info *p, size_t psize, void *opaque)
{
   char *cachepath = (char *) opaque;
   assert( cachepath );
   if (!p->dlpi_name || p->dlpi_name[0] == '\0')
      return 0;
   checkPathForLeak("dl_iterate_phdr", p->dlpi_name, cachepath);
   return 0;
}

static int check_proc_maps(char *path, char *cachepath)
{
   int fd, error, result;
   struct stat statbuf;
   char *maps = NULL;
   size_t filesize, pos = 0;

   fd = open(path, O_RDONLY);
   if (fd == -1) {
      error = errno;
      err_printf("Failed to open %s: %s\n", path, strerror(error));
      return -1;
   }

   result = fstat(fd, &statbuf);
   if (result == -1) {
      error = errno;
      err_printf("Failed to stat %s: %s\n", path, strerror(error));
      close(fd);
      return -1;
   }
   filesize = statbuf.st_size;

   maps = (char *) malloc(filesize + 1);
   do {
      result = read(fd, maps+pos, filesize-pos);
      if (result == -1 && errno == EINTR)
         continue;
      if (result == -1) {
         error = errno;
         err_printf("Failed to read from %s: %s\n", path, strerror(error));
         close(fd);
         return -1;
      }
      pos += result;
   } while (pos < filesize);
   maps[filesize] = '\0';
   close(fd);

   if (strstr(maps, cachepath)) {
      err_printf("Found leaked spindle path '%s' in maps '%s'\n", cachepath, path);
      return -1;
   }

   free(maps);   
   return 0;
}

void check_for_path_leaks()
{
   char *cachepath = NULL;
   DIR *proc_fds = NULL;
   struct dirent *d;
   char path[4096];
   struct link_map *lm;
   char *dlerr_msg = NULL;

   cachepath = getCacheLocation("LDCS_CHOSEN_PARSED_CACHEPATH");
   if (!cachepath) {
      err_printf("Failed to calculate cache location");
      goto done;
   }

   /**
    * Check symlinks in /proc/self/exe and /proc/self/fd/[*]  for leaks of spindle paths.
    **/
   proc_fds = opendir("/proc/self/fd");
   if (!proc_fds) {
      err_printf("Could not open directory /proc/self/fd");
      goto done;
   }
   for (d = readdir(proc_fds); d != NULL; d = readdir(proc_fds)) {
      if (d->d_name[0] == '.')
         continue;
      // Ignore Spindle fifo files for now.
      if ( strncmp( "315", d->d_name, 3 ) == 0 )
          continue;
      if ( strncmp( "316", d->d_name, 3 ) == 0 )
          continue;
      strncpy(path, "/proc/self/fd/", sizeof(path));
      strncat(path, d->d_name, sizeof(path)-1);
      checkLinkForLeak(path, cachepath);
   }
   checkLinkForLeak("/proc/self/exe", cachepath);

   /**
    * Check link_maps for leaked spindle paths
    **/
   for (lm = _r_debug.r_map; lm != NULL; lm = lm->l_next) {
      if (!lm->l_name || lm->l_name[0] == '\0')
         continue;
      checkPathForLeak("link_map", lm->l_name, cachepath);
   }

   /**
    * Check libraries in dl_iterate_phdr for leaked paths
    **/
   dl_iterate_phdr(leak_check_cb, cachepath);

   /**
    * Check /proc/pid/maps under various aliases for leaked names
    **/
   check_proc_maps("/proc/self/maps", cachepath);
   snprintf(path, sizeof(path), "/proc/self/task/%d/maps", getpid());
   check_proc_maps(path, cachepath);
   snprintf(path, sizeof(path), "/proc/%d/maps", getpid());
   check_proc_maps(path, cachepath);

   /**
    * Check that dlerror doesn't leak the /__not_exists/ prefix
    **/
   dlopen("libnosuchlib.so", RTLD_NOW);
   dlerr_msg = dlerror();
   if (dlerr_msg && strstr(dlerr_msg, "/__not_exists/")) {
      err_printf("Found not exists message in dlerror message: %s\n", dlerr_msg);
   }
   
  done:
   if (cachepath)
      free(cachepath);
   if (proc_fds)
      closedir(proc_fds);
}

void close_libs()
{
   int i, result;
   for (i=0; i<num_libraries; i++) {
      if (!libraries[i].dlhandle)
         continue;
      result = dlclose(libraries[i].dlhandle);
      if (result != 0)
         err_printf("Failed to close library %s: %s\n", libraries[i].libname, dlerror());
   }
}

int run_test()
{
   /*Make set_lib_functions gets called when a library loads.
     if a library has already been loaded, set_lib_functions
     will be called now. */
   setup_func_callback(set_lib_functions);

   if (chdir_mode)
      push_cwd();

   open_libraries();
   if (had_error)
      return -1;

   run_execs();
   if (had_error)
      return -1;

   run_stats();
   if (had_error)
      return -1;

   run_readlinks();
   if (had_error)
      return -1;

   run_alias_tests();
   if (had_error)
      return -1;
   
   check_libraries();
   if (had_error)
      return -1;

   call_funcs();
   if (had_error)
      return -1;

   check_for_path_leaks();
   if (had_error)
      return -1;

   checkTlsSum();
   if (had_error)
      return -1;

   close_libs();
   if (had_error)
      return -1;

   if (chdir_mode)
      pop_cwd();

   return 0;
}

int hash_start_val = 5381;
int hash(char *str)
{
   int hash = hash_start_val;
   int c;
   
   while ((c = *str++))
      hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
   
   hash_start_val += 7;
   return hash;
}

#if defined(USE_NEEDED)
volatile int dont_actually_call = 0;
int t10_calc();
int t11_calc();
int t12_calc();
int t13_calc();
int t14_calc();
int t15_calc();
int t16_calc();
int t17_calc();
int t18_calc();
int t19_calc();
int t20_calc();
int t50_calc();
int t100_calc();
int t500_calc();
int t1000_calc();
int t2000_calc();
int t4000_calc();
int t6000_calc();
int t8000_calc();
int t10000_calc();
int depA_calc();
int cxxexceptA_calc();
int origin_calc();
void reference_SOs()
{
   if (!dont_actually_call)
      return;
   t10_calc();
   t11_calc();
   t12_calc();
   t13_calc();
   t14_calc();
   t15_calc();
   t16_calc();
   t17_calc();
   t18_calc();
   t19_calc();
   t20_calc();
   t50_calc();
   t100_calc();
   t500_calc();
   t1000_calc();
   t2000_calc();
   t4000_calc();
   t6000_calc();
   t8000_calc();
   t10000_calc();
   depA_calc();
   cxxexceptA_calc();
   origin_calc();   
}
#else
void reference_SOs()
{
}
#endif

int main(int argc, char *argv[])
{
   int result = 0;
   int rank = -1;
   int passed;

   if (getenv("SPINDLE_TEST") == NULL) {
      fprintf(stderr, "Enable environment variable SPINDLE_TEST before running!\n");
      return -1;
   }

   reference_SOs();

   open_libraries_t *cur_lib;
   for (cur_lib = libraries; cur_lib->libname; cur_lib++, num_libraries++);

   parse_args(argc, argv);

   if (!nompi_mode) {
#if defined(HAVE_MPI)
      clean_local_libs();
      result = MPI_Init(&argc, &argv);
      MPI_Comm_rank(MPI_COMM_WORLD, &rank);
#else
      rank = 0;
#endif
   }
   

   /* Setup */
   if (fork_mode)
      setup_forkmode();

   /* Run test */
   result = run_test();

   /* Check results */
   passed = (result == 0);
   if (fork_mode && collect_forkmode(passed) == -1)
      passed = 0;
   if (!nompi_mode && !collectResults())
      passed = 0;

#if defined(HAVE_MPI)
   if (!nompi_mode) {
      MPI_Finalize();
      clean_local_libs();
   }
#endif

   if (rank == 0) {
      if (passed)
         printf("PASSED.\n");
      else
         printf("FAILED.\n");
   }

   if (abort_mode) {
      if (nompi_mode)
         abort();
      else {
#if defined(HAVE_MPI)         
         MPI_Abort(MPI_COMM_WORLD, 0);
#else
         abort();
#endif
      }
   }

   return passed ? 0 : -1;
}

static void setup_forkmode() {
   fork_child = fork();
   if (fork_child == -1) {
      err_printf("Unable to fork child process: %s\n", strerror(errno));
      return;
   }

   if (fork_child == 0) {
      fork_mode_is_child = 1;
      close(-1);
   }
   if (fork_child == 0 && forkexec_mode) {
      int i;
      char **newargv = (char **) malloc(sizeof(char *) * (gargc+1));
      for (i = 0; i < gargc; i++) {
         if (strstr(gargv[i], "fork")) {
            newargv[i] = "--nompi";
         }
         else
            newargv[i] = gargv[i];
      }
      newargv[gargc] = NULL;
      execv(newargv[0], newargv);
      err_printf("Failed to exec: %s\n", newargv[0]);
      _exit(-1);
   }
}

static int collect_forkmode(int passed) {
   if (!fork_child) {
      _exit(passed ? 0 : -1);
   }
   else {
      int status, result;
      do {
         result = waitpid(fork_child, &status, 0);
      } while (result == -1 && errno == EINTR);
      if (WIFSIGNALED(status)) {
         err_printf("Forked child %d exited with signal %d\n", fork_child, WTERMSIG(status));
         return -1;
      }
      else if (WIFEXITED(status)) {
         if (WEXITSTATUS(status) == 0) {
            return 0;
         }
         else {
            err_printf("Forked child %d exited with status %d\n", fork_child, WEXITSTATUS(status));
            return -1;
         }
      }
      else {
         assert(0);
      }
   }
   return -1;
}

/**
 * One process from each node in an MPI job will return true,
 * others will return false.
 **/
#if defined(HAVE_MPI)
static int getUniqueHostPerNode()
{
   int color, global_rank;
   int size, rank;
   int set_oldcomm = 0;
   MPI_Comm newcomm, oldcomm = MPI_COMM_WORLD;

   MPI_Comm_rank(MPI_COMM_WORLD, &global_rank);

   for (;;) {
      char name[MAX_STR_SIZE], oname[MAX_STR_SIZE];
      memset(name, 0, MAX_STR_SIZE);
      gethostname(name, MAX_STR_SIZE);
      color = hash(name);
      if (color < 0)
         color *= -1;
      
      int result = MPI_Comm_split(oldcomm, color, global_rank, &newcomm);
      if (result != MPI_SUCCESS) {
         fprintf(stderr, "Error in MPI_Comm_split\n");
         MPI_Abort(MPI_COMM_WORLD, -1);
      }
      
      if (set_oldcomm) {
         MPI_Comm_free(&oldcomm);
      }

      MPI_Comm_rank(newcomm, &rank);
      MPI_Comm_size(newcomm, &size);
      
      if (rank == 0)
         memcpy(oname, name, MAX_STR_SIZE);
      result = MPI_Bcast(oname, MAX_STR_SIZE, MPI_CHAR, 0, newcomm);
      if (result != MPI_SUCCESS) {
         fprintf(stderr, "Error in MPI_Scatter\n");
         MPI_Abort(MPI_COMM_WORLD, -1);
      }

      int global_str_match = 0;
      int str_match = (strcmp(name, oname) == 0);
      result = MPI_Allreduce(&str_match, &global_str_match, 1, MPI_INT, MPI_LAND, newcomm);
      if (result != MPI_SUCCESS) {
         fprintf(stderr, "Error in MPI_Allreduce\n");
         MPI_Abort(MPI_COMM_WORLD, -1);
      }

      if (global_str_match) {
         break;
      }
      
      set_oldcomm = 1;
      oldcomm = newcomm;
   }
   
   int result = MPI_Barrier(MPI_COMM_WORLD);
   if (result != MPI_SUCCESS) {
      fprintf(stderr, "Error in MPI_Barrier\n");
      MPI_Abort(MPI_COMM_WORLD, -1);
   }

   return (rank == 0);
}
#else
static int getUniqueHostPerNode()
{
   return 1;
}
#endif


static int collectResults()
{
   int is_unique = getUniqueHostPerNode();
   int test_passed = 1;
   char filename_passed[MAX_STR_SIZE];
   char filename_failed[MAX_STR_SIZE];

   if (is_unique) {
      char *tempdir = getenv("TMPDIR");
      if (!tempdir)
         tempdir = getenv("TEMPDIR");
      if (!tempdir)
         tempdir = "/tmp";

      snprintf(filename_passed, MAX_STR_SIZE, "%s/spindle_test_passed", tempdir);
      filename_passed[MAX_STR_SIZE-1] = '\0';
      snprintf(filename_failed, MAX_STR_SIZE, "%s/spindle_test_failed", tempdir);
      filename_failed[MAX_STR_SIZE-1] = '\0';

      unlink(filename_passed);
      unlink(filename_failed);
      test_printf("done\n");
      int timeout = 100; //10 seconds
      for (;;) {
         struct stat buf;
         if (stat(filename_passed, &buf) != -1) {
            test_passed = 1;
            break;
         }
         if (stat(filename_failed, &buf) != -1) {
            test_passed = 0;
            break;
         }
         if (timeout-- == 0) {
            fprintf(stderr, "[%s:%u] - Timeout waiting for test result\n", __FILE__, __LINE__);
            test_passed = 0;
            break;
         }
         usleep(100000);
      }
      unlink(filename_passed);
      unlink(filename_failed);
   }

   int global_test_passed = 0;
#if defined(HAVE_MPI)
   int result = MPI_Allreduce(&test_passed, &global_test_passed, 1, MPI_INT, MPI_LAND, MPI_COMM_WORLD);
   if (result != MPI_SUCCESS) {
      fprintf(stderr, "Error in MPI_Allreduce #2\n");
      MPI_Abort(MPI_COMM_WORLD, -1);
   }
#else
   global_test_passed = test_passed;
#endif
   return global_test_passed;
}
   
GCC7_ENABLE_WARNING
