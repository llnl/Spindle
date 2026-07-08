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

#if !defined PLUGIN_UTILS_H_
#define PLUGIN_UTILS_H_

#include <slurm/spank.h>
#include "spindle_launch.h"
#include <stdio.h>

#if defined(DEBUG)
#define sdprintf(PRI, FORMAT, ...) spindle_debug_printf(PRI, FORMAT, ##  __VA_ARGS__)
#else
#define sdprintf(PRI, FORMAT, ...)
#endif
#define err_printf(FORMAT, ...) sdprintf(1, "ERROR: " FORMAT, ## __VA_ARGS__)
#define debug_printf(FORMAT, ...) sdprintf(1, FORMAT, ## __VA_ARGS__)
#define debug_printf2(FORMAT, ...) sdprintf(2, FORMAT, ## __VA_ARGS__)
#define debug_printf3(FORMAT, ...) sdprintf(3, FORMAT, ## __VA_ARGS__)


char *encodeSpindleConfig(uint32_t port, uint32_t num_ports, uint64_t unique_id, uint32_t security_type,
                          int spindle_argc, char **spindle_argv);
int decodeSpindleConfig(const char *encodedstr,
                        unsigned int *port, unsigned int *num_ports, uint64_t *unique_id, uint32_t *security_type,
                        int *spindle_argc, char ***spindle_argv);

int srunAllNodes(unsigned int num_nodes, const char *command);
char **getHostsScontrol(unsigned int num_hosts, const char *hoststr);
char **getHostAddrSinfo(unsigned int num_hosts, char **hostlist);
char **getHostsParse(unsigned int num_hosts, const char *shortlist);

int isFEHost(char **hostlist, unsigned int num_hosts);
extern char *unique_file;
void cleanup_unique_file();
int isBEProc(spindle_args_t *params, unsigned int exit_phase);

int doesFEExitSocketExist(spindle_args_t *params);
int waitForSpankSessionEnd(spindle_args_t *params);
int signalSpankSessionEnd(spindle_args_t *params);

char *encodeCmdArgs(int sargc, char **sargv);
void decodeCmdArgs(char *cmd, int *sargc, char ***sargv);

typedef struct {
   char *new_home;
   char *old_home;
   char *new_path;
   char *old_path;
   char *new_pwd;
   char *old_pwd;
   char *new_tmpdir;
   char *old_tmpdir;
   char *new_spindledebug;
   char *old_spindledebug;
} saved_env_t;

void push_env(spank_t spank, saved_env_t **env);
void pop_env(saved_env_t *env);

int grandchild_fork();

typedef int (*dpr_function_t)(void *input, char **output_str);
int dropPrivilegeAndRun(dpr_function_t func, uid_t uid, void *input, char **output_str);

int registerFEPid(pid_t pid, spindle_args_t *args);
int readFEPid(pid_t *pid, spindle_args_t *args);

int signalFE(pid_t fepid);
int waitForSignalFE();
int prepWaitForSignalFE();
int superclose();
char *readSpankEnv(spank_t spank, const char *envname);
   
#endif
