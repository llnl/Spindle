/*
This file is part of Spindle.  For copyright information see the COPYRIGHT 
file in the top level directory, or at 
https://github.com/hpc/Spindle/blob/master/COPYRIGHT

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License (as published by the Free Software
Foundation) version 2.1 dated February 1999.  This program is distributed in the
hope that it will be useful, but WITHOUT ANY WARRANTY; without even the IMPLIED
WARRANTY OF MECHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the terms 
and conditions of the GNU Lesser General Public License for more details.  You should 
have received a copy of the GNU Lesser General Public License along with this 
program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/inotify.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <elf.h>

#include "ldcs_api.h"
#include "ldcs_cache.h"
#include "ldcs_api_listen.h"
#include "ldcs_audit_server_process.h"
#include "ldcs_audit_server_filemngt.h"
#include "ldcs_elf_read.h"
#include "config.h"
#include "ccwarns.h"
#include "cleanup_proc.h"
#include "fileutil.h"
#include "config.h"

#if !defined(LIBEXECDIR)
#error LIBEXECDIR must be defined
#endif

char *_ldcs_audit_server_cachepath;
static char *normalized_tmpdir;
static char *_ldcs_audit_server_commpath;
extern int spindle_mkdir(char *path);

static char *filemngt_normalize_dir(char *dir) {
   char *newpath = realpath(dir, NULL);
   return newpath ? newpath : dir;
}

int ldcs_audit_server_filemngt_init (char *cachepath, char *commpath) {
   int rc=0;

   debug_printf2("Setting cachepath to '%s' and commpath to '%s'\n", cachepath, commpath);
   _ldcs_audit_server_cachepath = cachepath;
   _ldcs_audit_server_commpath  = commpath;
   if (-1 == spindle_mkdir(_ldcs_audit_server_cachepath)) {
      err_printf("mkdir: ERROR during mkdir %s\n", _ldcs_audit_server_cachepath);
      _error("mkdir failed");
   }
   normalized_tmpdir = filemngt_normalize_dir(cachepath);
   
   return(rc);
}

/* Returns NULL if not a cached file. Otherwise, returns pointer to global portion of string */
char* ldcs_is_a_cachedfile (char* filename) {
  int len = strlen(_ldcs_audit_server_cachepath);
  int norm_len = strlen(normalized_tmpdir);

  if ( strncmp(_ldcs_audit_server_cachepath, filename, len) == 0 )
     return filename + len + 1;
  if ( strncmp(normalized_tmpdir, filename, norm_len) == 0 )
     return filename + norm_len + 1;
  return NULL;
}

extern char **parse_colonsep_prefixes(char *colonsep_list, number_t number);
extern int is_local_prefix(const char *path, char **cached_local_prefixes);
int ldcs_is_a_localfile(ldcs_process_data_t *procdata, char *filename) {
   static char **cached_localprefixes = NULL;
   if (!cached_localprefixes) {
      cached_localprefixes = parse_colonsep_prefixes(procdata->localprefix, procdata->number);
   }
   return is_local_prefix(filename, cached_localprefixes);
}


#define MAX_FILENAME_LEN 256
char *filemngt_calc_localname(char *global_name, calc_local_t reqtype)
{
   //The naming decisions here need to be cordinated with name parsing in
   // cache/global_name.c
   static unsigned int unique_str_num = 0;
   char target[MAX_PATH_LEN+1];
   char dirpart[MAX_PATH_LEN+1];
   char filepart[MAX_PATH_LEN+1];
   char *endslash, *lastslash;
   const char *prefix = NULL;
   size_t dirpart_size, filepart_size;
   int cut_dirpart_slash;

   lastslash = strrchr(_ldcs_audit_server_cachepath, '/');
   if (lastslash && lastslash[1] == '\0')
      endslash = "";
   else
      endslash = "/";

   GCC7_DISABLE_WARNING("-Wformat-truncation");
   
   lastslash = strrchr(global_name, '/');
   if (!lastslash) {
      debug_printf("WARNING: Got global name without a slash: %s\n", global_name);
      snprintf(dirpart, sizeof(dirpart), "unknown_path");
      dirpart[sizeof(dirpart)-1] = '\0';
      strncpy(filepart, global_name, sizeof(filepart));
      filepart[sizeof(filepart)-1] = '\0';
   }
   else {
      dirpart_size = lastslash - global_name;
      filepart_size = strlen(lastslash+1);
      assert(filepart_size < sizeof(filepart));
      assert(dirpart_size < sizeof(dirpart));
      
      strncpy(dirpart, global_name, dirpart_size);
      dirpart[dirpart_size] = '\0';
      strncpy(filepart, lastslash+1, sizeof(filepart));
      filepart[filepart_size] = '\0';
   }

   switch(reqtype) {
      case clt_unknown: prefix = "spindlens-unknown"; break;
      case clt_stat: prefix = "spindlens-stat"; break;
      case clt_lstat: prefix = "spindlens-lstat"; break;
      case clt_ldso: prefix = "spindlens-ldso"; break;
      case clt_file: prefix = "spindlens-file"; break;
      case clt_dso: prefix = "spindlens-dso"; break;
      case clt_numafile: prefix = "spindlens-numafile-XXXXXX"; break;
      case clt_numadso: prefix = "spindlens-numadso-XXXXXX"; break;
   }
   
   snprintf(target, sizeof(target), "%x-%s-%s",
            unique_str_num++,
            prefix,
            filepart);
   strncpy(filepart, target, sizeof(filepart));
   filepart[sizeof(filepart)-1] = '\0';

   cut_dirpart_slash = (dirpart[0] == '/') ? 1 : 0;
   
   snprintf(target, sizeof(target), "%s%s%s", _ldcs_audit_server_cachepath, endslash, dirpart+cut_dirpart_slash);
   spindle_mkdir(target);

   snprintf(target, sizeof(target), "%s%s%s/%s", _ldcs_audit_server_cachepath, endslash, dirpart+cut_dirpart_slash, filepart);

   GCC7_ENABLE_WARNING;
      
   return strdup(target);
}

int filemngt_read_file(char *filename, void *buffer, size_t *size, int strip,
                       int *errcode, int *was_stripped)
{
   FILE *f;
   int result = 0;

   debug_printf2("Reading file %s from disk\n", filename);
   f = fopen(filename, "r");
   if (!f) {
      *errcode = errno;
      debug_printf2("Could not read file %s from disk, errcode = %d\n", filename, *errcode);
      return 0;
   }
   int was_elf;
   result = read_file_and_strip(f, buffer, size, strip, 0, &was_elf);
   if (result == -1)
      err_printf("Error reading from file %s: %s\n", filename, strerror(errno));

   *was_stripped = (strip && was_elf);
   
   fclose(f);
   return result;
}

int filemngt_encode_packet(char *filename, void *filecontents, size_t filesize, int stripped,
                           char **buffer, size_t *buffer_size)
{
   size_t cur_pos = 0;
   int filename_len = strlen(filename) + 1;
   int is_elf = filemngt_is_elf_file(filecontents, filesize);
   //TODO: Remove filesize from allocation if we're doing a non-contig send. Wastes memory.
   *buffer_size = sizeof(is_elf) + sizeof(stripped) + filename_len + sizeof(filename_len) + sizeof(filesize) + filesize;
   *buffer = (char *) malloc(*buffer_size);
   if (!*buffer) {
      err_printf("Failed to allocate memory for file contents packet for %s\n", filename);
      return -1;
   }

   memcpy(*buffer + cur_pos, &is_elf, sizeof(is_elf));
   cur_pos += sizeof(is_elf);

   memcpy(*buffer + cur_pos, &stripped, sizeof(stripped));
   cur_pos += sizeof(stripped);
   
   memcpy(*buffer + cur_pos, &filename_len, sizeof(filename_len));
   cur_pos += sizeof(filename_len);

   memcpy(*buffer + cur_pos, &filesize, sizeof(filesize));
   cur_pos += sizeof(filesize);

   memcpy(*buffer + cur_pos, filename, filename_len);
   cur_pos += filename_len;

   /* Explicitely removing the memcpy that puts the file contents into the
      packet.  In order to keep file contents zero-copy we won't add them
      to the packet, but will instead send them with a second write command.
      memcpy(*buffer + cur_pos, filecontents, filesize);
   */
   cur_pos += filesize;

   assert(cur_pos == *buffer_size);
   return 0;
}

int filemngt_decode_packet(node_peer_t peer, ldcs_message_t *msg, char *filename, size_t *filesize, size_t *bytes_read, int *is_elf, int *stripped)
{
   int filename_len = 0;
   int result;

   if (!msg->data) {
      /* We've delayed the file read from the network.  Just read the elf, filename and size here.
         We'll later get the file contents latter by reading directly to mapped memory */
      result = ldcs_audit_server_md_complete_msg_read(peer, msg, is_elf, sizeof(*is_elf));
      if (result == -1)
         return -1;

      result = ldcs_audit_server_md_complete_msg_read(peer, msg, stripped, sizeof(*stripped));
      if (result == -1)
         return -1;
      
      result = ldcs_audit_server_md_complete_msg_read(peer, msg, &filename_len, sizeof(filename_len));
      if (result == -1)
         return -1;
      assert(filename_len > 0 && filename_len <= MAX_PATH_LEN+1);

      result = ldcs_audit_server_md_complete_msg_read(peer, msg, filesize, sizeof(*filesize));
      if (result == -1)
         return -1;
      
      result = ldcs_audit_server_md_complete_msg_read(peer, msg, filename, filename_len);
      if (result == -1)
         return -1;

      *bytes_read = sizeof(filename_len) + sizeof(*filesize) + filename_len;
   }
   else {
      size_t pos = 0;
      unsigned char *data = (unsigned char *) msg->data;
      *is_elf = *((int *) (data+pos));
      pos += sizeof(int);

      *stripped = *((int *) (data+pos));
      pos += sizeof(int);
      
      filename_len = *((int *) (data+pos));
      pos += sizeof(int);
      assert(filename_len > 0 && filename_len <= MAX_PATH_LEN+1);

      *filesize = *((size_t *) (data+pos));
      pos += sizeof(size_t);

      memcpy(filename, data+pos, filename_len);
      pos += filename_len;

      *bytes_read = pos;
   }
   return 0;
}

/**
 * Clear files from the local ramdisk
 **/
int ldcs_audit_server_filemngt_clean()
{
   cleanup_created_dirs(_ldcs_audit_server_cachepath, _ldcs_audit_server_commpath);
   return 0;
}

#define EMPTY_FILE_PTR ((void *) 0xE977)

int filemngt_create_file_space(char *filename, size_t size, void **buffer_out, int *fd_out)
{
   int result;
   *fd_out = open(filename, O_CREAT | O_EXCL | O_RDWR, 0700);
   if (*fd_out == -1) {
      err_printf("Could not create local file %s: %s\n", filename, strerror(errno));
      return -1;
   }
   if (size == 0) {
      debug_printf2("Empty file %s", filename);
      *buffer_out = EMPTY_FILE_PTR;
      return 0;
   }
   result = ftruncate(*fd_out, size);
   if (result == -1) {
      err_printf("Could not grow local file %s to %lu (out of memory?): %s\n", filename, size, strerror(errno));
      close(*fd_out);
      return -1;
   }
   *buffer_out = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, *fd_out, 0);
   if (*buffer_out == MAP_FAILED) {
      err_printf("Could not mmap file %s: %s\n", filename, strerror(errno));
      close(*fd_out);
      return -1;
   }
   assert(*buffer_out);
   return 0;
}

int filemngt_clear_file_space(void *buffer, size_t size, int fd)
{
   int result = 0;
   if (buffer && buffer != EMPTY_FILE_PTR && size)
      result = munmap(buffer, size);
   if (fd != -1)
      close(fd);
   if (result == -1) {
      err_printf("Error unmapping buffer");
      return -1;
   }

   return 0;
}

void *filemngt_sync_file_space(void *buffer, int fd, char *pathname, size_t size, size_t newsize)
{
   /* Linux gets annoying here.  We can't just mprotect the buffer to read-only,
      Linux considers the file still open for writing (and thus throws ETXTBUSY on exec's)
      as long as we still have the buffer mmaped from a fd that was opened with write.
      That means we've got to close the file, unmap the buffer, then re-open the file 
      read-only, then re-map the buffer to the same place.  Ugh.  
   */

   int result;
   char *buffer2;

   if (size == 0 || buffer == EMPTY_FILE_PTR) {
      debug_printf2("Empty file sync for %s", pathname);
      close(fd);
      return EMPTY_FILE_PTR;
   }

   debug_printf3("Unmapping buffer %p of size %lu\n", buffer, size);
   result = munmap(buffer, size);
   if (result == -1) {
      err_printf("Error unmapping buffer for %s\n", pathname);
      return NULL;
   }

   if (size != newsize) {
      assert(newsize < size);
      /* The file shrunk after we read it, probably because we stripped it
         as we read.  Shrink the local file on disk and shrink the mapped region */
      result = ftruncate(fd, newsize);
      if (result == -1) {
         err_printf("Could not shrink file in local disk\n");
         return NULL;
      }
   }
   
   close(fd);
   
   fd = open(pathname, O_RDONLY);
   if (fd == -1) {
      err_printf("Failed to re-open file %s: %s\n", pathname, strerror(errno));
      return NULL;
   }
   
   buffer2 = mmap(buffer, newsize, PROT_READ, MAP_SHARED | MAP_FIXED, fd, 0);
   if (buffer2 == MAP_FAILED) {
      debug_printf("Failure re-mapping file %s: %s. Retrying.\n", pathname, strerror(errno));
      buffer2 = mmap(NULL, newsize, PROT_READ, MAP_SHARED, fd, 0);
      if (buffer2 == MAP_FAILED) {
         debug_printf("Could not re-mapping file %s: %s\n", pathname, strerror(errno));
         return NULL;
      }
   }
   debug_printf3("Remapped buffer %p of size %lu as read-only at %p\n", buffer, size, buffer2);

   close(fd);

   return buffer2;
}

size_t filemngt_get_file_size(char *pathname, int *errcode)
{
   struct stat st;
   int result;

   result = stat(pathname, &st);
   if (result == -1) {
      if (errcode)
         *errcode = errno;
      debug_printf2("Could not stat file %s, perhaps bad symlink\n", pathname);
      return (size_t) -1;
   }
   return (size_t) st.st_size;
}

int filemngt_stat(char *pathname, extended_stat_t *buf, int is_lstat)
{
   int stat_result;
   int readlink_result;
   buf->readlink_path_size = 0;
   buf->readlink_errcode = 0;
   buf->readlink_path[0] = '\0';
   
   if (!is_lstat) {
      stat_result = stat(pathname, &buf->buf);
      debug_printf3("stat(%s) = %d\n", pathname, stat_result);
      return stat_result;
   }

   stat_result = lstat(pathname, &buf->buf);
   debug_printf3("lstat(%s) = %d\n", pathname, stat_result);
   if (stat_result != 0)
      return stat_result;
   if (buf->buf.st_mode & S_IFLNK) {
      memset(buf->readlink_path, 0, sizeof(buf->readlink_path));
      readlink_result = readlink(pathname, buf->readlink_path, sizeof(buf->readlink_path));
      if (readlink_result == -1) {
         buf->readlink_errcode = errno;
         buf->readlink_path_size = 0;
         debug_printf2("Readlink err %d while stating file %s\n",
                       buf->readlink_errcode, pathname);
      }
      else {
         buf->readlink_path[sizeof(buf->readlink_path)-1] = '\0';
         buf->readlink_path_size = strlen(buf->readlink_path) + 1;
         debug_printf2("Readlink %s while stating file %s\n", buf->readlink_path, pathname);
      }
   }

   return stat_result;
}

size_t extended_stat_size(extended_stat_t *st)
{
   size_t max = sizeof(extended_stat_t);
   size_t size = max - (sizeof(st->readlink_path) - st->readlink_path_size);
   size = size + 1;
   return size;
}

int filemngt_write_stat(char *localname, extended_stat_t *buf)
{
   int fd, result;
   fd = creat(localname, 0600);
   if (fd == -1) {
      err_printf("Failed to create file %s for writing: %s\n", localname, strerror(errno));
      close(fd);
      return -1;
   }

   result = write_n_bytes(localname, fd, &(buf->buf), sizeof(buf->buf));
   if (result == -1) {
      err_printf("Failed to write stat\n");
      close(fd);
      return -1;
   }

   result = write_n_bytes(localname, fd, &(buf->readlink_errcode), sizeof(buf->readlink_errcode));
   if (result == -1) {
      err_printf("Failed to write errcode\n");
      close(fd);
      return -1;
   }

   result = write_n_bytes(localname, fd, &(buf->readlink_path_size), sizeof(buf->readlink_path_size));
   if (result == -1) {
      err_printf("Failed to write result\n");
      close(fd);
      return -1;
   }
   
   result = write_n_bytes(localname, fd, &(buf->readlink_path), buf->readlink_path_size);
   if (result == -1) {
      err_printf("Failed to write path\n");
      close(fd);
      return -1;
   }
   close(fd);
   return 0;
}

int filemngt_write_ldsometadata(char *localname, ldso_info_t *ldsoinfo)
{
   return write_buffer(localname, (char *) ldsoinfo, sizeof(*ldsoinfo));
}

int filemngt_read_ldsometadata(char *localname, ldso_info_t *ldsoinfo)
{
   return read_buffer(localname, (char *) ldsoinfo, sizeof(*ldsoinfo));
}

int filemngt_read_stat(char *localname, extended_stat_t *buf)
{
   int fd, result;
   fd = open(localname, O_RDONLY);
   if (fd == -1) {
      err_printf("Failed to open %s for reading: %s\n", localname, strerror(errno));
      return -1;
   }

   result = read_n_bytes(localname, fd, &(buf->buf), sizeof(buf->buf));
   if (result == -1) {
      err_printf("Failed to read stat\n");
      close(fd);
      return -1;
   }

   result = read_n_bytes(localname, fd, &(buf->readlink_errcode), sizeof(buf->readlink_errcode));
   if (result == -1) {
      err_printf("Failed to read errcode\n");
      close(fd);
      return -1;
   }

   result = read_n_bytes(localname, fd, &(buf->readlink_path_size), sizeof(buf->readlink_path_size));
   if (result == -1) {
      err_printf("Failed to read result\n");
      close(fd);
      return -1;
   }

   if (buf->readlink_path_size > 0) {
      result = read_n_bytes(localname, fd, buf->readlink_path, buf->readlink_path_size);
      if (result == -1) {
         close(fd);
         return -1;
      }
   }
   else {
      buf->readlink_path[0] = '\0';
   }
   close(fd);
   return 0;
}

#if defined(arch_x86_64) || defined(arch_aarch64)
#define PROFILE_FUNC_NAME "_dl_runtime_profile"
#define GET_ADDR_FROM_SYMVALUE(X) (X)
#elif defined(arch_ppc64) || defined(arch_ppc64le)
#define PROFILE_FUNC_NAME "_dl_profile_resolve"
#define GET_ADDR_FROM_SYMVALUE(X) (*((unsigned long *) (X+base)))
#else
#error Unknown architecture
#endif

const char *stat_names[] = { "__xstat", "_xstat", "__xstat64", NULL };
const char *lstat_names[] = { "__lxstat", "_lxstat", "__lxstat64", NULL };
const char *errno_names[] = { "rtld_errno", NULL };

#define filemngt_ldso_elfx(filemngt_ldso_elfX, ElfX_Ehdr, ElfX_Shdr, ElfX_Sym, ElfX_Off, ElfX_Phdr) \
static int filemngt_ldso_elfX(unsigned char *base, ldso_info_t *ldsoinfo) \
{                                                                       \
   ElfX_Ehdr *ehdr;                                                     \
   ElfX_Shdr *shdr, *sec, *name_sec;                                    \
   ElfX_Sym *syms, *cur;                                                \
   ElfX_Off mem_offset, file_offset;                                    \
   ElfX_Phdr *phdrs, *phdr;                                             \
   unsigned int i, j, k, num_shdrs, num_syms, num_phdrs;                \
   int match_resolve, match_profile;                                    \
   char *names, *name;                                                  \
   unsigned long resolve_offset = 0, profile_offset = 0;                \
   unsigned long stat_offset = 0;                                       \
   unsigned long lstat_offset = 0;                                      \
   unsigned long errno_offset = 0;                                      \
                                                                        \
   ehdr = (ElfX_Ehdr *) base;                                           \
   shdr = (ElfX_Shdr *) (base + ehdr->e_shoff);                         \
   num_shdrs = ehdr->e_shnum;                                           \
   num_phdrs = ehdr->e_phnum;                                           \
   phdrs = (ElfX_Phdr *) (base + ehdr->e_phoff);                        \
                                                                        \
   for (i = 0; i < num_shdrs; i++) {                                    \
      sec = shdr + i;                                                   \
      if (sec->sh_type != SHT_SYMTAB && sec->sh_type != SHT_DYNSYM)     \
         continue;                                                      \
                                                                        \
      name_sec = shdr + sec->sh_link;                                   \
      names = (char *) (base + name_sec->sh_offset);                    \
                                                                        \
      syms = (ElfX_Sym *) (base + sec->sh_offset);                      \
      num_syms = sec->sh_size / sizeof(*syms);                          \
                                                                        \
      for (j = 0; j < num_syms; j++) {                                  \
         cur = syms + j;                                                \
         name = names + cur->st_name;                                   \
                                                                        \
         if (!stat_offset) {                                            \
            for (k = 0; stat_names[k]; k++) {                           \
               if (strcmp(name, stat_names[k]) == 0) {                  \
                  stat_offset = cur->st_value;                          \
               }                                                        \
            }                                                           \
         }                                                              \
         if (!lstat_offset) {                                           \
            for (k = 0; lstat_names[k]; k++) {                          \
               if (strcmp(name, lstat_names[k]) == 0) {                 \
                  lstat_offset = cur->st_value;                         \
               }                                                        \
            }                                                           \
         }                                                              \
         if (!errno_offset) {                                          \
            for (k = 0; errno_names[k]; k++) {                          \
               if (strcmp(name, errno_names[k]) == 0) {                 \
                  errno_offset = cur->st_value;                         \
               }                                                        \
            }                                                           \
         }                                                              \
         match_resolve = (strcmp(name, "_dl_runtime_resolve") == 0);    \
         match_profile = (strcmp(name, PROFILE_FUNC_NAME) == 0);        \
                                                                        \
         if (!match_resolve && !match_profile)                          \
            continue;                                                   \
                                                                        \
         file_offset = 0;                                               \
         mem_offset = cur->st_value;                                    \
         for (k = 0; k < num_phdrs; k++) {                              \
            phdr = phdrs + k;                                           \
            if (phdr->p_type != PT_LOAD)                                \
               continue;                                                \
            if (mem_offset >= phdr->p_vaddr && mem_offset < phdr->p_vaddr + phdr->p_memsz) { \
               file_offset = (mem_offset - phdr->p_vaddr) + phdr->p_offset; \
               break;                                                   \
            }                                                           \
         }                                                              \
         if (file_offset == 0) {                                        \
            err_printf("Error translating memory offset to file offset in ld.so symbol lookup\n"); \
            return -1;                                                  \
         }                                                              \
                                                                        \
         if (match_resolve)                                             \
            resolve_offset = GET_ADDR_FROM_SYMVALUE(file_offset);       \
         else if (match_profile)                                        \
            profile_offset = GET_ADDR_FROM_SYMVALUE(file_offset);       \
                                                                        \
         if (resolve_offset && profile_offset)                          \
            break;                                                      \
      }                                                                 \
      if (resolve_offset && profile_offset)                             \
         break;                                                         \
   }                                                                    \
                                                                        \
   if (!resolve_offset) {                                               \
      debug_printf("WARNING: Could not find symbol _dl_runtime_resolve in dynamic linker\n"); \
   }                                                                    \
                                                                        \
   if (!profile_offset) {                                               \
      debug_printf("WARNING: Could not find symbol _dl_runtime_profile in dynamic linker\n"); \
   }                                                                    \
                                                                        \
   if (stat_offset) {                                                   \
      debug_printf("WARNING: Could not find symbol stat in dynamic linker\n"); \
   }                                                                    \
                                                                        \
   if (!lstat_offset) {                                                 \
      debug_printf("WARNING: Could not find symbol fstat in dynamic linker\n"); \
   }                                                                    \
                                                                        \
   ldsoinfo->binding_offset = (signed long) (resolve_offset - profile_offset); \
   ldsoinfo->stat_offset = stat_offset;                                 \
   ldsoinfo->lstat_offset = lstat_offset;                               \
   ldsoinfo->errno_offset = errno_offset;                               \
   return 0;                                                            \
}

filemngt_ldso_elfx(filemngt_ldso_elf32, Elf32_Ehdr, Elf32_Shdr, Elf32_Sym, Elf32_Off, Elf32_Phdr)
filemngt_ldso_elfx(filemngt_ldso_elf64, Elf64_Ehdr, Elf64_Shdr, Elf64_Sym, Elf64_Off, Elf64_Phdr)

static int ldso_metadata_sym(char *pathname, ldso_info_t *ldsoinfo)
{
   int fd = -1, result, error, ret = -1;
   size_t ldso_size;
   struct stat buf;
   void *map_result = MAP_FAILED;
   char *elf_ident;

   fd = open(pathname, O_RDONLY);
   if (fd == -1) {
      error = errno;
      err_printf("Error opening linker path %s for metadata: %s\n", pathname, strerror(error));
      goto done;
   }

   result = fstat(fd, &buf);
   if (result == -1) {
      error = errno;
      err_printf("Error stating linker path %s for metadata: %s\n", pathname, strerror(error));
      goto done;
   }
   ldso_size = buf.st_size;

   map_result = mmap(NULL, ldso_size, PROT_READ, MAP_PRIVATE, fd, 0);
   if (map_result == MAP_FAILED) {
      error = errno;
      err_printf("Error mmaping linker for %s: %s\n", pathname, strerror(error));
      goto done;
   }

   elf_ident = (char *) map_result;
   if (strncmp(ELFMAG, elf_ident, SELFMAG) != 0) {
      err_printf("Error, linker %s was not an elf file\n", pathname);
      goto done;
   }

   if (elf_ident[EI_CLASS] == ELFCLASS32) {
      filemngt_ldso_elf32(map_result, ldsoinfo);
   }
   else if (elf_ident[EI_CLASS] == ELFCLASS64) {
      filemngt_ldso_elf64(map_result, ldsoinfo);      
   }
   else {
      err_printf("Error, linker %s had invalid elf class %d\n", pathname, (int) elf_ident[EI_CLASS]);
      goto done;
   }
   
   ret = 0;
  done:

   if (fd != -1)
      close(fd);
   if (map_result != MAP_FAILED)
      munmap(map_result, ldso_size);
   return ret;
}

#if !defined(os_bgq)
static int ldso_metadata_run(char *pathname, ldso_info_t *ldsoinfo)
{
   FILE *f;
   char *cmdline;
   unsigned int cmdline_size;
   int error, result;
   unsigned long resolve_offset, profile_offset;

   cmdline_size = 2 * (strlen(pathname) + strlen(LIBEXECDIR) + strlen("/print_ldso_entry") + 2) + 1;
   cmdline = (char *) malloc(cmdline_size);

   debug_printf3("Running '%s' to get ldso offsets\n", cmdline);
   snprintf(cmdline, cmdline_size, "%s %s %s %s", 
            pathname, LIBEXECDIR "/print_ldso_entry",
            pathname, LIBEXECDIR "/print_ldso_entry");
   f = popen(cmdline, "r");
   if (f == NULL) {
      error = errno;
      err_printf("Failed to run '%s' to get loader offets: %s\n", cmdline, strerror(error));
      free(cmdline);
      return -1;
   }
   free(cmdline);

   result = fscanf(f, "%lu\n%lu\n", &resolve_offset, &profile_offset);
   pclose(f);
   if (result != 2) {
      err_printf("Failed to read offsets from print_ldso_entry\n");
      return -1;
   }

   ldsoinfo->binding_offset = (signed long) (resolve_offset - profile_offset);
   ldsoinfo->stat_offset = 0;
   ldsoinfo->lstat_offset = 0;
   ldsoinfo->errno_offset = 0;
   return 0;
}
#endif

int filemngt_get_ldso_metadata(char *pathname, ldso_info_t *ldsoinfo)
{
   int result;

   debug_printf("Looking up symbol names in linker %s\n", pathname);
   result = ldso_metadata_sym(pathname, ldsoinfo);
   if (result == 0)
      return 0;

#if !defined(os_bgq)
   debug_printf("Getting symbol offsets of %s from invoking print_ldso_entry\n", pathname);
   result = ldso_metadata_run(pathname, ldsoinfo);
   if (result == 0)
      return 0;
#endif

   err_printf("Could not find any mechanism for fetching ldso metadata of %s\n", pathname);
   return -1;
}

int filemngt_realpath(char *pathname, char *realfile)
{
   char *result;
   result = realpath(pathname, realfile);
   if (!result) {
      debug_printf("Warning: Could not resolve realpath of file %s\n", pathname);
      return -1;
   }

   return (strcmp(pathname, realfile) == 0) ? 0 : 1;
}

int filemngt_is_elf_file(const char *buffer, size_t buffer_size)
{
   if (buffer_size < 4)
      return 0;
   return ((buffer[0] == 0x7f) &&
           (buffer[1] == 'E') &&
           (buffer[2] == 'L') &&
           (buffer[3] == 'F'));
}


int filemngt_convert_proc_maps(int pid, char *new_maps_filename, int new_maps_filename_size)
{
   int result;
   debug_printf2("Asked to convert /proc/%d/maps to remove spindle paths\n", pid);
   
   result = translate_proc_pid_maps(_ldcs_audit_server_cachepath, pid, new_maps_filename, new_maps_filename_size);
   if (result == -1) {
      new_maps_filename[0] = '\0';
      return -1;
   }
   
   debug_printf2("Converted /proc/%d/maps to %s\n", pid, new_maps_filename);

   return 0;
}
