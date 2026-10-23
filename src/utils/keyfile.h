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

#if !defined(KEYFILE_H_)
#define KEYFILE_H_

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

void create_key(unsigned char *buffer, int key_size_bytes);
void get_keyfile_path(char *pathname, int pathname_len, uint64_t unique_id);
void create_keyfile(uint64_t unique_id);
void clean_keyfile(uint64_t unique_id);
const char* get_sec_keydir_path(void);

#if defined(__cplusplus)
}
#endif

#endif
