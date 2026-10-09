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

#include "crash_sigchain.h"
#include "crash_arch.h"
#include "intercept.h"
#include "spindle_debug.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sched.h>
#include <linux/fs.h>

/* This file deals with signal handler chaining. If the application
   has registered its own signal handler, we want to run that one first,
   before Spindle runs its own. This turns out to be rather complicated,
   because the application might do something in its signal handler that
   fixes the fault, making it no longer a crash that Spindle should handle.
   As an example, the Java and Julia runtimes implement garbage collection
   "safepoints" (points in the code where the GC is allowed to run) as reads
   to PROT_NONE pages. The signal handler then waits for all threads to reach
   a safepoint, runs the GC, then unprotects the page and returns, allowing the
   faulting instruction to re-execute. If Spindle ran its own signal handler
   after this, safepoints would crash. As another example, libsigsegv uses
   a SIGSEGV handler to implement user-level paging. Much of the complexity in
   this file comes from handling cases such as these. */

/* CAUTION: signal and sigaction are on the async-signal-safe list, so the wrappers
   themselves must also be async-signal-safe! */

typedef void (*sighandler_t)(int);

void *orig_sigaction;
void *orig_signal;
void *orig_bsd_signal;
void *orig_sysv_signal;

struct app_disposition {
   void (*handler)(int);
   void (*sigaction_fn)(int, siginfo_t *, void *);
   sigset_t mask;
   int flags;
   int has_sigaction;
   int active;
   int ignored;
};

static const int OWNED_SIGS[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };

/* Set once the crash handler has installed its handlers. */
static int sigchain_active = 0;

void crash_sigchain_activate(void)
{
   sigchain_active = 1;
}

int crash_sigchain_is_owned(int sig)
{
   if (!sigchain_active)
      return 0;
   for (size_t i = 0; i < sizeof(OWNED_SIGS) / sizeof(OWNED_SIGS[0]); i++) {
      if (OWNED_SIGS[i] == sig)
          return 1;
   }
   return 0;
}

/* Storage for the application's registered signal handlers */
static struct app_disposition disp_store[NSIG];

/* Spinlock to protect access to stored app signal dispositions.*/
static volatile int disp_lock[NSIG];

static void crash_disp_lock(int sig)
{
   while (__sync_lock_test_and_set(&disp_lock[sig], 1)) {
      sched_yield();
   }
}

static void crash_disp_unlock(int sig)
{
   __sync_lock_release(&disp_lock[sig]);
}

void crash_sigchain_reset_locks(void)
{
   int sig;
   for (sig = 0; sig < NSIG; sig++)
      __sync_lock_release(&disp_lock[sig]);
}

/* Caller must hold disp lock for signal */
static void store_disposition(int sig, const struct sigaction *act)
{
   if (act == NULL) return;
   struct app_disposition *d = &disp_store[sig];
   if (act->sa_handler == SIG_DFL || act->sa_handler == SIG_IGN) {
      d->active = 0;
      d->ignored = (act->sa_handler == SIG_IGN);
      return;
   }
   d->ignored = 0;
   d->has_sigaction = (act->sa_flags & SA_SIGINFO) ? 1 : 0;
   if (d->has_sigaction) {
      d->sigaction_fn = act->sa_sigaction;
      d->handler = NULL;
   } else {
      d->handler = act->sa_handler;
      d->sigaction_fn = NULL;
   }
   d->mask  = act->sa_mask;
   d->flags = act->sa_flags & ~(SA_SIGINFO | SA_ONSTACK | SA_RESTART);
   d->active = 1;
}

/* Caller must hold disp lock for signal */
static void get_disposition(int sig, struct sigaction *out)
{
   struct app_disposition *d = &disp_store[sig];
   memset(out, 0, sizeof *out);
   if (!d->active) {
      out->sa_handler = d->ignored ? SIG_IGN : SIG_DFL;
      return;
   }
   if (d->has_sigaction) {
      out->sa_sigaction = d->sigaction_fn;
      out->sa_flags     = d->flags | SA_SIGINFO;
   } else {
      out->sa_handler = d->handler;
      out->sa_flags   = d->flags;
   }
   out->sa_mask = d->mask;
}

/* This is called during initialization to register a signal handler that
   was already registered before Spindle's crash handler was registered */
void crash_sigchain_register_existing_handler(int sig, const struct sigaction *handler_old)
{
   if (!crash_sigchain_is_owned(sig)) return;
   crash_disp_lock(sig);
   store_disposition(sig, handler_old);
   crash_disp_unlock(sig);
   /* This function is not called from a signal handler, so it's safe to log */
   debug_printf2("stored application signal handler for sig %d\n", sig);
}

/* Run the application signal handler, if present.  Returns 
     - CRASH_CHAIN_NONE if the disposition is SIG_DFL
     - CRASH_CHAIN_IGNORED if it is SIG_IGN
     - CRASH_CHAIN_HANDLED if there was an application handler 
       and we successfully ran it and returned from it. */
int crash_sigchain_chain_to_app(int sig, siginfo_t *info, void *ucontext)
{
   struct app_disposition local;

   crash_disp_lock(sig);

   /* If there's no application handler, nothing more for us to do */
   if (!disp_store[sig].active) {
      int ignored = disp_store[sig].ignored;
      crash_disp_unlock(sig);
      return ignored ? CRASH_CHAIN_IGNORED : CRASH_CHAIN_NONE;
   }

   /* Make a local copy of the signal disposition so we can release the lock
      before calling the stored signal handler, as it might call sigaction itself */
   local = disp_store[sig];

   /* If the app requested the handler be reset, do so */
   if (local.flags & SA_RESETHAND) {
      disp_store[sig].active = 0;
   }
   crash_disp_unlock(sig);

   /* Build the mask we want active during the application's handler */
   sigset_t blockset = local.mask;
   if (!(local.flags & SA_NODEFER)) sigaddset(&blockset, sig);
   sigset_t saved;
   (void) sigprocmask(SIG_BLOCK, &blockset, &saved);

   /* Call the application's signal handler */ 
   if (local.has_sigaction) {
      local.sigaction_fn(sig, info, ucontext);
   } else {
      local.handler(sig);
   }

   /* If we reach here, the application handler returned.
    Restore the mask to its previous value. */
   (void) sigprocmask(SIG_SETMASK, &saved, NULL);
   return CRASH_CHAIN_HANDLED;
}

/* Fault resolution detection
   This section contains functions used to detect whether the application's
   signal handler fixed the fault or not. */

#ifdef PROCMAP_QUERY
/* Linux 6.11 and later let us query /proc/pid/maps without textual
   parsing. This stores whether we can do that. */
static int crash_maps_query_supported = 0;
#endif

/* Probe whether addr is readable in this process by attempting a
   1-byte process_vm_readv from a local stack buffer.  Returns
      1 if the read succeeded
      0 if the read faultedd
     -1 if the read failed for some reason other than EFAULT
   We use this to detect if the application has fixed the reason for
   a read fault (e.g., for the Java/Julia safepoint pattern).
  */
static inline int crash_probe_byte_readable(const void *addr)
{
   char buf;
   struct iovec local  = { &buf, 1 };
   struct iovec remote = { (void *) addr, 1 };
   /* process_vm_readv is intended to let a process read the memory
      of its ptrace target, but it can also be used to read one's own
      memory. In the event of a fault, it returns EFAULT instead of
      triggering a signal. */
   long n = syscall(SYS_process_vm_readv,
                    (long) getpid(),
                    &local, 1UL,
                    &remote, 1UL,
                    0UL);
   if (n == 1) return 1; // Successfully read 1 byte
   if (n == -1 && errno == EFAULT) return 0; // Faulted
   return -1; // Neither successfully read nor faulted
}

/* Checking whether the application handler fixed a write fault is
   much more annoying than the read case. For reads, we can safely
   just retry the read and see if it succeeded. For writes, however,
   we can't safely retry without actually writing into the application's
   memory. So for the write case, we instead check /proc/self/maps to see
   whether the address in question is writable after the application
   handler returns. */

#ifdef PROCMAP_QUERY
/* On Linux 6.11 and later, we can read /proc/self/maps via ioctl
   instead of having to parse it ourselves. */
static inline int crash_maps_find_via_ioctl(int fd, uintptr_t addr,
                                            struct crash_map *map)
{
   struct procmap_query q;
   memset(&q, 0, sizeof q);
   q.size = sizeof q;
   q.query_addr = (uint64_t) addr;
   q.query_flags = 0;
   if (map->path) {
      q.vma_name_addr = (uint64_t) (uintptr_t) map->path;
      q.vma_name_size = map->path_size;
   }
   int rc;
   do {
      rc = ioctl(fd, PROCMAP_QUERY, &q);
   } while (rc < 0 && errno == EINTR);
   if (rc < 0) {
      if (errno == ENOENT)
         return 0;
      return -1;
   }
   map->start = q.vma_start;
   map->end = q.vma_end;
   map->offset = q.vma_offset;
   map->writable = (q.vma_flags & PROCMAP_QUERY_VMA_WRITABLE) ? 1 : 0;
   if (map->path && q.vma_name_size == 0)
      map->path[0] = '\0';
   return 1;
}
#endif /* PROCMAP_QUERY */

/* On Linux kernels older than 6.11, the ioctl doesn't exist and we
   have to parse /proc/self/maps using only async-signal-safe functions. */

/* Hex digit value, or -1 if c is not a hex digit.
   We need this because strtoul is not on the async-signal-safe list. */
static inline int hexval(char c)
{
   if (c >= '0' && c <= '9') return c - '0';
   if (c >= 'a' && c <= 'f') return c - 'a' + 10;
   if (c >= 'A' && c <= 'F') return c - 'A' + 10;
   return -1;
}

/* Parse /proc/self/maps looking for the range containing addr.
   Lines look like
      start-end perms offset dev inode      path
   Returns 1 if found, 0 if not mapped, and -1 on error. */
static int crash_maps_find_via_textparse(int fd, uintptr_t addr,
                                         struct crash_map *map)
{
   const size_t buf_size = 4096;
   char *buf = (char *) syscall(SYS_mmap, NULL, buf_size,
                                PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS,
                                -1, 0);
   if (buf == MAP_FAILED)
      return -1;

   enum { ADDR_START, ADDR_END, PERMS, SKIP_PERMS, OFFSET, DEV_INODE,
          PATH_START, PATH, SKIP_EOL } state = ADDR_START;
   uintptr_t start = 0, end = 0;
   unsigned long offset = 0;
   int perm_idx = 0, writable = 0, spaces = 0;
   size_t path_len = 0;
   int result = 0;
   int done = 0;

   while (!done) {
      ssize_t n;
      do {
          n = syscall(SYS_read, fd, buf, buf_size);
      } while (n < 0 && errno == EINTR);
      if (n < 0) { 
          result = -1;
          break; 
      }
      if (n == 0) // EOF
          break;

      for (ssize_t i = 0; i < n && !done; i++) {
         char c = buf[i];
         switch (state) {
         /* We begin parsing a line with the start address of the range */
         case ADDR_START:
            if (c == '-') {
                /* When we get to the '-', change to parsing the end address */
                state = ADDR_END;
            } else {
                /* Accumulate one hex digit at at time */
                start = (start << 4) | hexval(c);
            }
            break;
         case ADDR_END:
            if (c == ' ') { 
                /* When we get to a space, we're done with the range and permissions are next */
                state = PERMS;
                perm_idx = 0; 
            } else {
                /* Accumulate one hex digit at at time */
                end = (end << 4) | hexval(c);
            }
            break;
         case PERMS:
            /* perms are "rwxp". We only care about the 'w' byte at index 1. */
            if (perm_idx++ == 1) {
                writable = (c == 'w');
                /* Now that we have the range and writability, check if our
                   address is in this range */
                if (start > addr) {
                    done = 1;
                } else if (addr < end) {
                    /* Found it. */
                    result = 1;
                    state = SKIP_PERMS;
                } else {
                    state = SKIP_EOL;
                }
            }
            break;
         case SKIP_PERMS:
            if (c == ' ')
                state = OFFSET;
            break;
         case OFFSET:
            if (c == ' ')
                state = DEV_INODE;
            else
                /* Accumulate one hex digit at at time */
                offset = (offset << 4) | hexval(c);
            break;
         case DEV_INODE:
            /* Skip the dev and inode fields */
            if (c == ' ' && ++spaces == 2)
                state = PATH_START;
            break;
         case PATH_START:
            /* The path is padded with spaces, and absent for anonymous memory */
            if (c == ' ')
                break;
            state = PATH;
            /* fall through */
         case PATH:
            if (c == '\n')
                done = 1;
            else if (map->path && path_len + 1 < map->path_size)
                map->path[path_len++] = c;
            else
                path_len = map->path_size;
            break;
         case SKIP_EOL:
            if (c == '\n') {
                /* When we get to the end of the line, set up to parse the next line */
                state = ADDR_START;
                start = 0;
                end = 0; 
            }
            break;
         }
      }
   }

   (void) syscall(SYS_munmap, buf, buf_size);
   if (result == 1) {
      map->start = start;
      map->end = end;
      map->offset = offset;
      map->writable = writable;
      if (map->path)
         map->path[path_len < map->path_size ? path_len : 0] = '\0';
   }
   return result;
}

int crash_maps_find(uintptr_t addr, struct crash_map *map)
{
   int fd = syscall(SYS_openat, AT_FDCWD, "/proc/self/maps", O_RDONLY | O_CLOEXEC);
   if (fd < 0) return -1;

   int rc;
#ifdef PROCMAP_QUERY
   if (crash_maps_query_supported)
      rc = crash_maps_find_via_ioctl(fd, addr, map);
   else
#endif
      rc = crash_maps_find_via_textparse(fd, addr, map);

   (void) syscall(SYS_close, fd);
   return rc;
}

/* Probe whether addr is writable in this process by checking
   /proc/self/maps. Returns
      1 if the address is mapped in a writable page
      0 if the address is not mapped in a writable page
     -1 if an error occurred reading the maps file
   We use this to detect if the application has fixed the reason for
   a write fault (e.g., libsigsegv for user-level paging).
  */
static inline int crash_maps_check_writable(uintptr_t addr)
{
   struct crash_map map = { 0 };
   int rc = crash_maps_find(addr, &map);
   if (rc != 1)
      return rc;
   return map.writable;
}

void crash_sigchain_init(void)
{
#ifdef PROCMAP_QUERY
   int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
   if (fd >= 0) {
      struct crash_map map = { 0 };
      if (crash_maps_find_via_ioctl(fd, (uintptr_t) &crash_maps_query_supported, &map) == 1)
         crash_maps_query_supported = 1;
      (void) close(fd);
   }
#endif /* PROCMAP_QUERY */
}

/* The main entrypoint to the fault recovery detection. */
int crash_sigchain_fault_resolved(int sig, siginfo_t *info, void *uctx,
                                  unsigned long pc_before)
{
   /* PC advanced inside the application handler. */
   if (extract_pc(uctx) != pc_before)
      return 1;

   /* The remaining possible fixes only apply to faulting on an
      addresss (SIGSEGV, SIGBUS). */
   if (sig != SIGSEGV && sig != SIGBUS)
      return 1;

   /* A user-sent signal has no faulting instruction to retry.
      Rather, the application handler returning consumes it. */
   if (info->si_code <= 0)
      return 1;

   /* If we faulted on a write, check if the address is now mapped writable.
      If we faulted on a read, retry the read and check if we refault. */
   int rc = pf_is_write(uctx) ? crash_maps_check_writable((uintptr_t) info->si_addr)
                              : crash_probe_byte_readable(info->si_addr);

   /* Only a probe that positively saw the fault still present (0) means
      unresolved.  If the probe itself could not run, treat as handled,
      which preserves the behavior without Spindle. */
   return rc != 0;
}

/* sigaction-family wrappers */

int sigaction_wrapper(int sig, const struct sigaction *act,
                      struct sigaction *oldact)
{
   if (!crash_sigchain_is_owned(sig)) {
      return ((int (*)(int, const struct sigaction *, struct sigaction *))
              orig_sigaction)(sig, act, oldact);
   }

   crash_disp_lock(sig);
   if (oldact != NULL) get_disposition(sig, oldact);
   store_disposition(sig, act);
   crash_disp_unlock(sig);
   return 0;
}

static sighandler_t signal_common(int sig, sighandler_t handler,
                                  int flags, void *orig)
{
   if (!crash_sigchain_is_owned(sig)) {
      return ((sighandler_t (*)(int, sighandler_t)) orig)(sig, handler);
   }

   struct sigaction act, oldact;
   memset(&act, 0, sizeof act);
   act.sa_handler = handler;
   act.sa_flags = flags;
   (void) sigaction_wrapper(sig, &act, &oldact);

   return (oldact.sa_flags & SA_SIGINFO) ? SIG_DFL : oldact.sa_handler;
}

void (*signal_wrapper(int sig, void (*handler)(int)))(int)
{
   return signal_common(sig, handler, SA_RESTART, orig_signal);
}

void (*bsd_signal_wrapper(int sig, void (*handler)(int)))(int)
{
   return signal_common(sig, handler, SA_RESTART, orig_bsd_signal);
}

void (*sysv_signal_wrapper(int sig, void (*handler)(int)))(int)
{
   return signal_common(sig, handler, SA_RESETHAND | SA_NODEFER,
                        orig_sysv_signal);
}
