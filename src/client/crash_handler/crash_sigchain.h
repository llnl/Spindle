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

#if !defined(CRASH_SIGCHAIN_H_)
#define CRASH_SIGCHAIN_H_

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

/* Return values of crash_sigchain_chain_to_app */
#define CRASH_CHAIN_NONE     0  /* no application handler */
#define CRASH_CHAIN_HANDLED  1  /* application handler ran and returned */
#define CRASH_CHAIN_IGNORED  2  /* application disposition is SIG_IGN */

void crash_sigchain_activate(void);
int crash_sigchain_is_owned(int sig);
void crash_sigchain_register_existing_handler(int sig, const struct sigaction *handler_old);
int crash_sigchain_chain_to_app(int sig, siginfo_t *info, void *ucontext);
void crash_sigchain_init(void);
void crash_sigchain_reset_locks(void);
int crash_sigchain_fault_resolved(int sig, siginfo_t *info, void *uctx,
                                  unsigned long pc_before);

/* Values from /proc/self/maps used in crash handling */
struct crash_map {
   uintptr_t start, end;
   unsigned long offset;
   int writable;
   char *path;
   size_t path_size;
};

int crash_maps_find(uintptr_t addr, struct crash_map *map);

int sigaction_wrapper(int sig, const struct sigaction *act,
                      struct sigaction *oldact);
void (*signal_wrapper(int sig, void (*handler)(int)))(int);
void (*bsd_signal_wrapper(int sig, void (*handler)(int)))(int);
void (*sysv_signal_wrapper(int sig, void (*handler)(int)))(int);

#endif
