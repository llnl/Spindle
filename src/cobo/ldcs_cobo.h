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

/*
 * ==========================================================================
 * This library enables distributed processes to bootstrap themselves through
 * a series of collective operations.  The collective operations are modeled
 * after MPI collectives -- all tasks must call them in the same order and with
 * consistent parameters.
 *
 * Any number of collectives may be invoked, in any order, passing an arbitrary
 * amount of data.  All message sizes are specified in bytes.
 *
 * All functions return COBO_SUCCESS on successful completion.
 * ==========================================================================
*/

#ifndef _COBO_H
#define _COBO_H

#if defined(__cplusplus)
extern "C" {
#endif

#include <stdint.h>
#include "handshake.h"

#define COBO_SUCCESS (0)

#define COBO_NAMESPACE ldcs

#if defined(COBO_NAMESPACE)
#define COMBINE2(a, b) a ## _ ## b
#define COMBINE(a, b) COMBINE2(a, b)
#define cobo_open COMBINE(COBO_NAMESPACE, cobo_open)
#define cobo_close COMBINE(COBO_NAMESPACE, cobo_close)
#define cobo_get_parent_socket COMBINE(COBO_NAMESPACE, cobo_get_parent_socket)
#define cobo_barrier COMBINE(COBO_NAMESPACE, cobo_barrier)
#define cobo_bcast COMBINE(COBO_NAMESPACE, cobo_bcast)
#define cobo_gather COMBINE(COBO_NAMESPACE, cobo_gather)
#define cobo_scatter COMBINE(COBO_NAMESPACE, cobo_scatter)
#define cobo_allgather COMBINE(COBO_NAMESPACE, cobo_allgather)
#define cobo_alltoall  COMBINE(COBO_NAMESPACE, cobo_alltoall )
#define cobo_allgather_str COMBINE(COBO_NAMESPACE, cobo_allgather_str)
#define cobo_allreduce COMBINE(COBO_NAMESPACE, cobo_allreduce)
#define cobo_server_open COMBINE(COBO_NAMESPACE, cobo_server_open)
#define cobo_server_close COMBINE(COBO_NAMESPACE, cobo_server_close)
#define cobo_server_get_root_socket COMBINE(COBO_NAMESPACE, cobo_server_get_root_socket)
#define __cobo_ts COMBINE(COBO_NAMESPACE, __cobo_ts)
#define cobo_get_num_childs COMBINE(COBO_NAMESPACE, cobo_get_num_childs)
#define cobo_bcast_down COMBINE(COBO_NAMESPACE, cobo_bcast_down)
#define cobo_get_child_socket COMBINE(COBO_NAMESPACE, cobo_get_child_socket)
#define cobo_set_handshake COMBINE(COBO_NAMESPACE, cobo_set_handshake)
#define cobo_get_startup_timeout_ms COMBINE(COBO_NAMESPACE, cobo_get_startup_timeout_ms)
#define cobo_now_ms COMBINE(COBO_NAMESPACE, cobo_now_ms)
#define cobo_read_fd_w_timeout COMBINE(COBO_NAMESPACE, cobo_read_fd_w_timeout)
#define cobo_write_fd_w_suppress COMBINE(COBO_NAMESPACE, cobo_write_fd_w_suppress)
#define cobo_preconnect_cb_t COMBINE(COBO_NAMESPACE, cobo_preconnect_cb_t)
#define cobo_register_preconnect_cb COMBINE(COBO_NAMESPACE, cobo_register_preconnect_cb)
#endif

// Used for cobo_allreduce().
typedef enum{
    COBO_OP_MIN,
    COBO_OP_MAX,
    COBO_OP_BITWISE_AND,
    COBO_OP_BITWISE_OR,
    COBO_OP_LOGICAL_AND,
    COBO_OP_LOGICAL_OR,
    COBO_OP_SUM,
    COBO_OP_NOOP,
    NUM_COBO_OP
} cobo_op_t;

/*
 * ==========================================================================
 * ==========================================================================
 * Client Interface Functions
 * ==========================================================================
 * ==========================================================================
 */

/* provide list of ports and number of ports as input, get number of tasks and my rank as output */
int cobo_open(uint64_t sessionid, int* portlist, int num_ports, int* rank, int *num_ranks);

/* shut down the connections between tasks and free data structures */
int cobo_close();

/* fills in fd with socket file desriptor to our parent */
/* TODO: the upside here is that the upper layer can directly use our
 * communication tree, but the downside is that it exposes the implementation
 * and forces sockets */
int cobo_get_parent_socket(int* fd);

/* sync point, no task makes it past until all have reached */
int cobo_barrier();

/* root sends sendcount bytes from buf, each task recevies sendcount bytes into buf */
int cobo_bcast    (void* buf, int sendcount, int root);

/* like bcast, but every task takes a message rather than receives one */
int cobo_bcast_down(void *buf, int sendcount);

/* each task sends sendcount bytes from buf, root receives N*sendcount bytes into recvbuf */
int cobo_gather   (void* sendbuf, int sendcount, void* recvbuf, int root);

/* root sends blocks of sendcount bytes to each task indexed from sendbuf */
int cobo_scatter  (void* sendbuf, int sendcount, void* recvbuf, int root);

/* each task sends sendcount bytes from sendbuf and receives N*sendcount bytes into recvbuf */
int cobo_allgather(void* sendbuf, int sendcount, void* recvbuf);

/* each task sends N*sendcount bytes from sendbuf and receives N*sendcount bytes into recvbuf */
int cobo_alltoall (void* sendbuf, int sendcount, void* recvbuf);
   
/*
 * Perform MPI-like Allgather of NULL-terminated strings (whose lengths may vary
 * from task to task).
 *
 * Each task provides a pointer to its NULL-terminated string as input.
 * Each task then receives an array of pointers to strings indexed by rank number
 * and also a pointer to the buffer holding the string data.
 * When done with the strings, both the array of string pointers and the
 * buffer should be freed.
 *
 * Example Usage:
 *   char host[256], **hosts, *buf;
 *   gethostname(host, sizeof(host));
 *   cobo_allgatherstr(host, &hosts, &buf);
 *   for(int i=0; i<nprocs; i++) { printf("rank %d runs on host %s\n", i, hosts[i]); }
 *   free(hosts);
 *   free(buf);
 */
int cobo_allgather_str(char* sendstr, char*** recvstr, char** recvbuf);

int cobo_allreduce(uint64_t *pval, cobo_op_t op);

/*
 * ==========================================================================
 * ==========================================================================
 * Server Interface Functions
 * ==========================================================================
 * ==========================================================================
 */

/* given a hostlist and portlist where clients are running, open the tree and assign ranks to clients */
int cobo_server_open(uint64_t sessionid, char** hostlist, int num_hosts, int* portlist, int num_ports);

/* shut down the tree connections (leaves processes running) */
int cobo_server_close();

/* fills in fd with socket file desriptor to the root client process (rank 0) */
/* TODO: the upside here is that the upper layer can directly use our
 * communication tree, but the downside is that it exposes the implementation
 * and forces sockets */
int cobo_server_get_root_socket(int* fd);

extern double __cobo_ts;

int cobo_get_num_childs(int* num_childs);

/* Methods to access child fds */
int cobo_get_child_socket(int num, int *fd);

void cobo_set_handshake(handshake_protocol_t *hs);

/* Startup time limit in milliseconds (COBO_CONNECT_TIMELIMIT, default 60s) */
int cobo_get_startup_timeout_ms(void);

/* Monotonic clock in milliseconds */
long long cobo_now_ms(void);

/* Read size bytes from fd within timeout_ms overall, no limit if negative */
int cobo_read_fd_w_timeout(int fd, void* buf, int size, int timeout_ms);

/* Write size bytes to fd; with suppress, errors are written as debug logs instead */
int cobo_write_fd_w_suppress(int fd, void* buf, int size, int suppress);

void handle_security_error(const char *msg);
int initialize_handshake_security(handshake_protocol_t *protocol);

typedef int (*cobo_preconnect_cb_t)(const char *);
void cobo_register_preconnect_cb(cobo_preconnect_cb_t f);
   
#if defined(__cplusplus)
}
#endif

#endif /* _COBO_H */
