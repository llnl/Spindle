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

#include <locale.h>
#include <unistd.h>

#include "spindle_debug.h"
#include "config.h"

#if defined(NEWTHREAD_LOCALE_BUG)
#include <sys/syscall.h>

static __thread int fixed_locale __attribute__((tls_model("initial-exec"))) = 0;

static pid_t gettid()
{
   return syscall(SYS_gettid);   
}

#endif

/**
 * This works around the LD_AUDIT bug documented at:
 *  https://sourceware.org/bugzilla/show_bug.cgi?id=32483
 *
 * We need to set the locale for app-space created threads in the audit space.
 * Otherwise we see seg faults under malloc in spindle when malloc calls
 * get_nprocs.
 **/
void check_for_new_thread()
{
#if defined(NEWTHREAD_LOCALE_BUG)
   locale_t l;
   if (fixed_locale)
      return;
   fixed_locale = 1;
   if (gettid() == getpid()) {
      debug_printf3("Primary thread. Not updating locale.\n");      
      return;
   }

   debug_printf2("Identified new thread. Fixing locale\n");
   l = uselocale((locale_t) 0);
   uselocale(l);
#endif
}
