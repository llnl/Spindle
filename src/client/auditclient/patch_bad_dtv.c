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
#include <link.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "client.h"
#include "auditclient.h"
#include "spindle_debug.h"


/**
 * This file works around a glibc problem when LD_AUDIT is enable, where the DTV (dynamic thread vector)
 * gets allocated using ld.so's rtld_malloc function, then later gets resized by the application's realloc function
 * (SPINDLE_SRC/scripts/dtvtest/ has a reproducer that can make this happen--you need lots of libraries with TLS). 
 * 
 * We'll work around this problem by A) recognizing when this will happen, and B) copying the DTV to a 
 * memory region properly controlled by the application's malloc.
 * We recognize this because in the buggy case, the DTV will change values twice before libc.so enters a 
 * consistent state. 
 * We copy the DTV to a proper malloc region by getting its pointer out of TLS space. DTV is a vector, and
 * dtv[-1] is the vector size, which makes it not-to-difficult to get copies. We're relying on the structure
 * of libc internals to do this safely, but those internals don't look to have changed over the lifetime of
 * this bug.
 **/


/**
 * This dtv_t structure is copied from glibc source internals. That's kind of not safe,
 * but also kind of safe because the structure is consistent through the lifetime of the
 * bug we're working around, and seems to have stayed consistent for even longer.
 **/
struct dtv_pointer
{
   void *val;
   void *to_free;
};
typedef union dtv
{
   size_t counter;
   struct dtv_pointer pointer;
} dtv_t;

typedef void* (*malloc_fptr_t)(size_t);

static dtv_t *initial_dtv;

static dtv_t *getDTV()
{
   if (!DTV_ALLOCATION_BUG)
      return NULL;
   
   void *result = NULL;
#if defined(__x86_64__)
   __asm__("mov %%fs:0x8, %%rax\n"
           "mov %%rax, %0\n"
           : "=r" (result)
           :
           : "rax");
#endif
   return result;
}

static void setDTV(dtv_t *newdtv)
{
#if defined(__x86_64__)
   __asm__("mov %0, %%rax\n"
           "mov %%rax, %%fs:0x8\n"
           :
           : "r" (newdtv)
           : "rax");
#endif
}

static dtv_t *reallocDTV(dtv_t *dtv, malloc_sig_t malloc_fptr)
{
   size_t oldsize = dtv[-1].counter;
   size_t newdtv_alloc_size = (2 + oldsize) * sizeof(dtv_t);
   dtv_t *newdtv = malloc_fptr(newdtv_alloc_size);
   memcpy(newdtv, dtv - 1, newdtv_alloc_size);
   setDTV(newdtv+1);
   return newdtv+1;
}

// Call on la_version
void patchDTV_init()
{
   initial_dtv = getDTV();
}

//Call on la_activity with LA_ACT_CONSISTENT
void patchDTV_check()
{
   static int checked_dtv = 0;
   if (checked_dtv)
      return;
   checked_dtv = 1;

   dtv_t *dtv = getDTV();
   if (!dtv)
      return;
   if (dtv == initial_dtv)
      return;

   debug_printf2("Reallocating dtv to work around glibc bug. initial_dtv = %p ; dtv = %p\n",
                 initial_dtv, dtv);
   malloc_sig_t app_malloc = get_app_malloc();
   if (!app_malloc) {
      debug_printf("Warning: Could not lookup up application malloc to realloc dtv. App may be prone to crashes during dlopen's\n");
      return;
   }
   dtv_t *newdtv = reallocDTV(dtv, app_malloc);
   debug_printf2("DTV was successfully reallocated to %p\n", newdtv);
}
