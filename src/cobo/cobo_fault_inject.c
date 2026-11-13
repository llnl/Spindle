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

/* COBO fault injection for startup resiliency testing */

#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include "cobo_fault_inject.h"

/* Check whether comma-separated list contains item. */
static int list_contains(const char *list, const char *item)
{
    size_t len = strlen(item);
    const char *p = list;
    while (p && *p) {
        const char *end = strchr(p, ',');
        size_t n = end ? (size_t) (end - p) : strlen(p);
        if (n == len && strncmp(p, item, len) == 0)
            return 1;
        p = end ? end + 1 : NULL;
    }
    return 0;
}

/* Stall until the parent closes or the stall time elapses */
static void stall(void)
{
    const char *secs_s = getenv("SPINDLE_TEST_BE_STALL_SECS");
    long long deadline = cobo_now_ms() + 1000LL * (secs_s ? atoi(secs_s) : 300);
    struct pollfd pfd;
    int parent_fd = -1;
    cobo_get_parent_socket(&parent_fd);
    pfd.fd = parent_fd;
    pfd.events = POLLRDHUP;
    for (;;) {
        long long remaining = deadline - cobo_now_ms();
        if (remaining <= 0)
            return;
        pfd.revents = 0;
        int rc = poll(&pfd, 1, (int) remaining);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc != 0)
            _exit(1);
    }
}

/* Handles fault points in cobo_open_tree and just before READY.
 * phase is either "tree" or "ready" to select the desired point. */
void cobo_test_fault_point(const char *phase, int rank)
{
    char rank_s[16];
    const char *selected_phase = getenv("SPINDLE_TEST_BE_FAULT_PHASE");
    const char *exit_list = getenv("SPINDLE_TEST_BE_EXIT");
    const char *stall_list = getenv("SPINDLE_TEST_BE_STALL");

    /* Fault injection is disabled unless SPINDLE_TEST_BE_FAULT_PHASE is set and
     * at least one of SPINDLE_TEST_BE_EXIT or SPINDLE_TEST_BE_STALL is set */
    if (!selected_phase || !*selected_phase)
        return;
    if ((!exit_list || !*exit_list) && (!stall_list || !*stall_list))
        return;

    /* Check if this is the right phase for the fault point. */
    if (strcmp(selected_phase, phase) != 0)
        return;

    /* Check if this is the right rank for the fault point. */
    snprintf(rank_s, sizeof(rank_s), "%d", rank);

    /* If this rank is listed in SPINDLE_TEST_BE_EXIT,
     * exit at the fault point. */
    if (exit_list && list_contains(exit_list, rank_s))
        _exit(1);

    /* If this rank is listed in SPINDLE_TEST_BE_STALL,
     * stall at the fault point. */
    if (stall_list && list_contains(stall_list, rank_s))
        stall();
}

/* The fault point at the beginning of cobo_open occurs before
 * ranks have been assigned and so uses hostname rather than 
 * rank. Later fault points use rank so we can make some ranks
 * within an node exit or stall while others proceed normally. */
void cobo_test_exit_hosts(void)
{
    char host[256];
    const char *list = getenv("SPINDLE_TEST_BE_EXIT_HOSTS");
    if (!list || gethostname(host, sizeof(host)) != 0)
        return;
    host[sizeof(host) - 1] = '\0';
    if (list_contains(list, host))
        _exit(1);
}
