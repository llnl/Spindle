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

#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <assert.h>
#include <stdio.h>

#include <set>
#include <string>
#include <map>
#include "parseloc.h"
#include "spindle_launch.h"
#include "spindle_debug.h"
#include "config.h"
#include "parse_launcher.h"

using namespace std;

extern bool setOpenMPIInterceptEnv(string launcher_rel);

/**
 * Setup library locations, which come from autoconf
 **/
#if !defined(PROGLIBDIR)
#error Expected PROGLIBDIR defined
#endif
char libstr_socket_subaudit[] = PROGLIBDIR "/libspindle_subaudit_socket.so";
char libstr_pipe_subaudit[] = PROGLIBDIR "/libspindle_subaudit_pipe.so";
char libstr_biter_subaudit[] = PROGLIBDIR "/libspindle_subaudit_biter.so";

char libstr_socket_audit[] = PROGLIBDIR "/libspindle_audit_socket.so";
char libstr_pipe_audit[] = PROGLIBDIR "/libspindle_audit_pipe.so";
char libstr_biter_audit[] = PROGLIBDIR "/libspindle_audit_biter.so";

char libstr_intercept_lib[] = PROGLIBDIR "/libspindleint.so";
#if defined(COMM_SOCKET)
static char *default_audit_libstr = libstr_socket_audit;
static char *default_subaudit_libstr = libstr_socket_subaudit;
#elif defined(COMM_PIPES)
static char *default_audit_libstr = libstr_pipe_audit;
static char *default_subaudit_libstr = libstr_pipe_subaudit;
#elif defined(COMM_BITER)
static char *default_audit_libstr = libstr_biter_audit;
static char *default_subaudit_libstr = libstr_biter_subaudit;
#else
#error Unknown connection type
#endif

SRunParser *srunparser;
SerialParser *serialparser;
OpenMPIParser *openmpiparser;
WreckRunParser *wreckrunparser;
JSRunParser *jsrunparser;
LRunParser *lrunparser;
MarkerParser *markerparser;

unsigned int default_launchers_enabled = 0
#if defined(ENABLE_SRUN_LAUNCHER)
   | srun_launcher
#endif
#if defined(ENABLE_OPENMPI_LAUNCHER)
   | openmpi_launcher
#endif
#if defined(ENABLE_WRECKRUN_LAUNCHER)
   | wreckrun_launcher
#endif
#if defined(ENABLE_JSRUN_LAUNCHER)
   | jsrun_launcher
#endif
#if defined(ENABLE_LRUN_LAUNCHER)
   | lrun_launcher
#endif
   | marker_launcher;

CmdLineParser::CmdLineParser(int argc_, char **argv_, LauncherParser *parser_) :
   argc(argc_),
   argv(argv_),
   launcher_at(-1),
   exec_at(-1),
   parser(parser_)
{
}

CmdLineParser::~CmdLineParser()
{
}

CmdLineParser::parse_ret_t CmdLineParser::parse()
{
   int cur = 0;

   if (spindle_debug_prints) {
      string cmdline = string(argv[0]);
      for (int i = 1; i < argc; i++) 
         cmdline = cmdline + string(" ") + string(argv[i]);
      debug_printf("Launcher Parsing: Using %s to parse command line:\n\t%s\n",
                    getParser()->getName().c_str(), cmdline.c_str());
   }
   //Locate the launcher command.  Usually at argv[0]
   if (parser->usesLauncher()) {
      for (cur = 0; cur < argc; cur++) {
         if (parser->isLauncher(argc, argv, cur)) {
            launcher_at = cur;
            break;
         }
      }
      if (launcher_at == -1)
         return no_launcher;
      cur++;
   }

   for (; cur < argc; cur++) {
      cmdoption_t *opt = parser->getArg(argc, argv, cur);
      if (!opt && parser->isExecutable(argc, argv, cur, exedirs)) {
         debug_printf2("Launcher Parsing: %s is the application executable\n", argv[cur]);
         exec_at = cur;
         return success;
      }
      else if (opt) {
         vector<string> argOptions;
         int inc_argc = 0;
         parser->getArgOptions(argc, argv, cur, opt, argOptions, inc_argc);
         if (spindle_debug_prints) {
            if (argOptions.empty()) 
               debug_printf2("Launcher Parsing: %s is a launcher argument\n", argv[cur]);
            else {
               debug_printf2("Launcher Parsing: %s is a launcher argument with values\n", argv[cur]);
               for (vector<string>::iterator j = argOptions.begin(); j != argOptions.end(); j++) {
                  debug_printf2("Launcher Parsing: %s is a argument value to %s\n", j->c_str(), argv[cur]);
               }
            }
         }
         
         if (opt->flags & FL_EXEDIR) {
            for (vector<string>::iterator j = argOptions.begin(); j != argOptions.end(); j++) {
               debug_printf2("Launcher Parsing: %s is a directory to use for executable searching\n", j->c_str());
               exedirs.insert(*j);
            }
         }
         if (opt->flags & FL_CUSTOM_OPTION) {
            debug_printf2("Launcher Parsing: %s is handled by a custom argument parser\n", argv[cur]);
            parser->parseCustomArg(argc, argv, cur, inc_argc);
         }
         cur += inc_argc;
      }
      else {
         debug_printf2("Launcher Parsing: Warning: %s is an unrecognized option.  Assuming it's a launcher option\n",
                       argv[cur]);
      }
   }

   return no_exec;
}

int CmdLineParser::launcherAt()
{
   return launcher_at;
}

int CmdLineParser::appExecutableAt()
{
   return exec_at;
}

LauncherParser *CmdLineParser::getParser()
{
   return parser;
}

ModifyArgv::ModifyArgv(int argc_, char **argv_,
                       int daemon_argc_, char **daemon_argv_,
                       spindle_args_t *params_) :
   argc(argc_),
   argv(argv_),
   new_argc(0),
   new_argv(NULL),
   daemon_argc(daemon_argc_),
   daemon_argv(daemon_argv_),
   params(params_),
   parser(NULL)
{
}

bool ModifyArgv::chooseParser()
{
   if (!params->use_launcher) {
      if (!autodetectParser())
         return false;
      return true;
   }

   set<LauncherParser *> parsers;
   initParsers(params->use_launcher, parsers);
   assert(parsers.size() == 1);
   parser = new CmdLineParser(argc, argv, *parsers.begin());
   return true;
}

bool ModifyArgv::autodetectParser()
{
   //Get all the LauncherParsers we've been configured with
   set<LauncherParser *> parsers;

   initParsers(default_launchers_enabled, parsers);
   if (parsers.empty()) {
      fprintf(stderr, "Error: Spindle was not configured with support for any MPI implementations.");
      return false;
   }
   
   //Get the list of CmdLineParsers that can work with our argv/argc
   set<CmdLineParser *> cmdline_parsers;
   for (set<LauncherParser*>::iterator i = parsers.begin(); i != parsers.end(); i++) {
      LauncherParser *lparser = *i;
      if (!lparser->valid(argc, argv))
         continue;
      cmdline_parsers.insert(new CmdLineParser(argc, argv, lparser)); 
   }

   //If there are multiple valid command line parsers that can work, then 
   // (e.g, mvapich and openmpi both use 'mpirun' as their launcher name),
   // we need to look more closely at the parsers.
   if (cmdline_parsers.size() != 1) {
      for (set<CmdLineParser *>::iterator i = cmdline_parsers.begin(); i != cmdline_parsers.end(); i++) {
         //If the marker_launcher is one of our set, then always use that one.
         CmdLineParser *cl_parser = *i;
         LauncherParser *lparser = cl_parser->getParser();
         if (lparser->getCode() == marker_launcher) {
            for (set<CmdLineParser *>::iterator j = cmdline_parsers.begin(); j != cmdline_parsers.end(); j++) {
               if (*j != cl_parser)
                  delete *j;
            }
            cmdline_parsers.clear();
            cmdline_parsers.insert(cl_parser);
            break;
         }
      }
   }
   if (cmdline_parsers.size() != 1) {
      for (set<CmdLineParser *>::iterator i = cmdline_parsers.begin(); i != cmdline_parsers.end(); ) {
         CmdLineParser *cl_parser = *i;
         LauncherParser *lparser = cl_parser->getParser();
         if (!lparser->valid2(argc, argv)) {
            set<CmdLineParser *>::iterator j = i;
            i++;
            delete *j;
            cmdline_parsers.erase(j);
         }
         else 
            i++;
      }      
   }
   if (cmdline_parsers.size() != 1) {
      print_err("Spindle could not identify the MPI implementation used on the command line");
      return false;
   }
   parser = *cmdline_parsers.begin();
   params->use_launcher = parser->getParser()->getCode();
   return true;
}

void ModifyArgv::print_err(string msg)
{
   fprintf(stderr, "%s\n", msg.c_str());
   fprintf(stderr, "\n");
   fprintf(stderr, "Try explicitely specifying the job launcher on the spindle command line.  E.g:\n");
   fprintf(stderr, " spindle -openmpi mpirun -np 4 a.out arg1 arg2\n");
   fprintf(stderr, "Alternatively, try adding the 'spindlemarker' keyword before your executable.  E.g:\n");
   fprintf(stderr, " spindle mpirun -np 4 spindlemarker a.out arg1 arg2\n\n");
}

int ModifyArgv::modifyCmdLine()
{
   char options_str[32];
   snprintf(options_str, 32, "%lu", (unsigned long) params->opts);
   string options(options_str);
   int rc;
   
   char number_str[32];
   snprintf(number_str, 32, "%lu", (unsigned long) params->number);
   string number(number_str);

   char daemon_argc_str[32];
   snprintf(daemon_argc_str, 32, "%u", daemon_argc);

   char shm_cache_size_str[32];
   snprintf(shm_cache_size_str, 32, "%u", params->shm_cache_size);
   string shmcache_size(shm_cache_size_str);

   const char *default_libstr = params->opts & OPT_SUBAUDIT ? default_subaudit_libstr : default_audit_libstr;
   const char *intercept_libstr = params->opts & OPT_SUBAUDIT ? libstr_intercept_lib : "";

   int new_argv_size = argc + 9 + daemon_argc;
   new_argv = (char **) malloc(sizeof(char *) * new_argv_size);
   
   int n = 0;
   int p = parser->launcherAt();
   if (p == -1) p = 0;

   for (; p < argc; p++) {
      if (p == parser->appExecutableAt()) {
#if defined(os_bluegene)
         string bg_env_str = parser->getParser()->getBGString();
         parser->getParser()->addBGEnvStr(n, new_argv, bg_env_str, default_libstr, intercept_libstr, params->commpaths, number, options, shmcache_size);
#else
         char **a_argv;
         int a_argc;
         rc = getApplicationArgsFE(params, &a_argc, &a_argv);
         if( rc != 0 ){
             return -1;
         }
         for (int i = 0; i < a_argc; i++) 
            new_argv[n++] = a_argv[i];
         (void) default_libstr; (void) intercept_libstr; //Not needed on linux
#endif
      }
      if (!parser->getParser()->includeArg(argc, argv, p))
         continue;
      new_argv[n++] = strdup(argv[p]);         
   }
   new_argv[n] = NULL;
   assert(n < new_argv_size);
   new_argc = n;
   return 0;
}

bool ModifyArgv::getNewArgv(int &newargc, char** &newargv)
{
   if (new_argv) {
      newargc = new_argc;
      newargv = new_argv;
      return true;
   }
   
   bool bresult = chooseParser();
   if (!bresult)
      return false;

   assert(parser);

   CmdLineParser::parse_ret_t result;
   result = parser->parse();
   switch (result) {
      case CmdLineParser::success:
         break;
      case CmdLineParser::no_exec:
         err_printf("Could not find executable in command line\n");
         print_err("Spindle was unable to locate an executable in your command line");
         return false;
      case CmdLineParser::no_launcher:
         err_printf("Could not find launcher in command line\n");
         print_err("Spindle was unable to find the job launcher command in your command line");
         return false;
   }
   
   int rc = modifyCmdLine();
   if( rc == -1 ){
       return false;
   }
   
   if (spindle_debug_prints) {
      string new_cmdline = string(new_argv[0]);
      for (int i = 1; i < new_argc; i++) {
         new_cmdline = new_cmdline + string(" ") + string(new_argv[i]);
      }
      debug_printf("Launcher Parsing: New command line is:\n\t%s\n", new_cmdline.c_str());
   }
   newargc = new_argc;
   newargv = new_argv;

   if (params->use_launcher == openmpi_launcher && params->startup_type == startup_lmon) {
      setOpenMPIInterceptEnv(argv[parser->launcherAt()]);
   }
   return true;
}
