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

#ifndef LDCS_AUDIT_SERVER_FILEMNGT_H
#define LDCS_AUDIT_SERVER_FILEMNGT_H

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ldcs_audit_server_md.h"

int ldcs_audit_server_filemngt_init (char *cachepath, char *commpath);

int filemngt_read_file(char *filename, void *buffer, size_t *size, int strip, int *err, int *was_stripped);
int filemngt_encode_packet(char *filename, void *filecontents, size_t filesize, 
                           int stripped, char **buffer, size_t *buffer_size);
int filemngt_decode_packet(node_peer_t peer, ldcs_message_t *msg, char *filename, size_t *buffer_size, size_t *bytes_read, int *is_elf, int *stripped);

typedef enum {
   clt_unknown,
   clt_stat,
   clt_lstat,
   clt_ldso,
   clt_file,
   clt_numafile,
   clt_dso,
   clt_numadso
} calc_local_t;

typedef struct 
{
   struct stat buf;
   int readlink_errcode;
   ssize_t readlink_path_size;
   char readlink_path[MAX_PATH_LEN+1];
} extended_stat_t;
size_t extended_stat_size(extended_stat_t *st);

char *filemngt_calc_localname(char *global_name, calc_local_t reqtype);
void filemngt_calc_dirname(const char *global_name, char *local_name_result, size_t local_name_result_size);
   
int ldcs_audit_server_filemngt_clean();

int filemngt_create_file_space(char *filename, size_t size, void **buffer_out, int *fd_out);
void *filemngt_sync_file_space(void *buffer, int fd, char *pathname, size_t size, size_t newsize);
int filemngt_clear_file_space(void *buffer, size_t size, int fd);
size_t filemngt_get_file_size(char *pathname, int *errcode);

char* ldcs_is_a_cachedfile(char* filename);
int ldcs_is_a_localfile(ldcs_process_data_t *procdata, char* filename);
int filemngt_stat(char *pathname, extended_stat_t *buf, int is_lstat);
int filemngt_write_stat(char *localname, extended_stat_t *buf);
int filemngt_read_stat(char *localname, extended_stat_t *buf);
int filemngt_write_ldsometadata(char *localname, ldso_info_t *ldsoinfo);
int filemngt_read_ldsometadata(char *localname, ldso_info_t *ldsoinfo);
int filemngt_is_elf_file(const char *buffer, size_t buffer_size);
int filemngt_get_ldso_metadata(char *pathname, ldso_info_t *ldsoinfo);
int filemngt_realpath(char *pathname, char *realfile);
int filemngt_convert_proc_maps(int pid, char *new_maps_filename, int new_maps_filename_size);
extern int translate_proc_pid_maps(char *output_dir, int pid, char *output_file, int output_file_size);
#endif
