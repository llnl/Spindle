#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <pwd.h>
#include "plugin_utils.h"

#if !defined(STR)
#define XSTR(X) #X
#define STR(X) XSTR(X)
#endif

#if defined SCONTROL_BIN
#define SLURM_SCONTROL_BIN STR(SCONTROL_BIN)
#else
#define SLURM_SCONTROL_BIN "scontrol"
#endif
#if defined SINFO_BIN
#define SLURM_SINFO_BIN STR(SINFO_BIN)
#else
#define SLURM_SINFO_BIN "sinfo"
#endif


extern char *parse_location(char *loc, number_t number);
extern char *realize(char *path);
extern int spindle_mkdir(char *orig_path);

int srunAllNodes(unsigned int num_nodes, const char *command) 
{
   pid_t pid;
   int result, status, error;

   pid = fork();
   if (pid == -1) {
      error = errno;
      sdprintf(1, "ERROR: Failed for fork for srun: %s\n", strerror(error));
      return -1;
   } else if (pid == 0) {
      // In child
      char n[12];
      snprintf(n, sizeof(n), "%u", num_nodes);
      execlp("srun",
             "srun", 
             "--nodes", n,
             "--ntasks", n,
             "--external-launcher",
             "--spindle",
             command,
             (char*)NULL);
      error = errno;
      sdprintf(1, "ERROR: Failed to exec srun: %s\n", strerror(error));
      exit(-1);
   } else {
      // In parent
      result = waitpid(pid, &status, 0);
      if (result == -1) {
         error = errno;
         sdprintf(1, "ERROR: Failed to wait for srun: %s\n", strerror(error));
         return -1;
      } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
         sdprintf(1, "ERROR: Failure in srun");
         return -1;
      }
   }
   return 0;
}

char **getHostsScontrol(unsigned int num_hosts, const char *hoststr)
{
   const char *scontrol_path = SLURM_SCONTROL_BIN;
   const char *scontrol_args = "show hostnames";
   const char *scontrol_suffix = "2> /dev/null";
   char *scontrol_cmdline;
   int scontrol_cmdline_len;
   FILE *f = NULL;
   char **hostlist = NULL, *s, **ret = NULL;
   int i, j, hostnamelen;
   int result;
   size_t len = 0;

   hostlist = (char **) calloc(num_hosts+1, sizeof(char*));

   scontrol_cmdline_len = strlen(scontrol_path) + strlen(scontrol_args) + strlen(hoststr) + strlen(scontrol_suffix) + 6;
   scontrol_cmdline = (char *) malloc(scontrol_cmdline_len);

   result = snprintf(scontrol_cmdline, scontrol_cmdline_len, "%s %s \"%s\" %s",
                     scontrol_path, scontrol_args, hoststr, scontrol_suffix);
   if (result >= scontrol_cmdline_len) {
      sdprintf(1, "ERROR: Fomatting error creating scontrol cmdline %s.  %d %d.\n",
               scontrol_cmdline, result, scontrol_cmdline_len);
      goto done;
   }
   sdprintf(2, "Running scontrol to get host list: %s\n", scontrol_cmdline);

   f = popen(scontrol_cmdline, "r");
   if (!f) {
      sdprintf(1, "ERROR: Could not run scontrol: %s\n", scontrol_cmdline);
      goto done;
   }

   i = 0;
   while (!feof(f)) {
      s = NULL;
      result = getline(&s, &len, f);
      if (result == -1 && feof(f)) {
         if (s)
            free(s);
         break;
      }
      else if (result == -1) {
         int error = errno;
         sdprintf(1, "ERROR: Could not getline from scontrol: %s\n", strerror(error));
         (void) error;
         goto done;
      }

      hostnamelen = strlen(s);
      for (j = 0; j < hostnamelen; j++) {
         if (!((s[j] >= '0' && s[j] <= '9') ||
               (s[j] >= 'a' && s[j] <= 'z') ||
               (s[j] >= 'A' && s[j] <= 'Z') ||
               (s[j] == '-' || s[j] == '_' || s[j] == '.'))) {
            s[j] = '\0';
            break;
         }
      }
      if (s[0] == '\0')
         continue;
      if (i >= num_hosts) {
         sdprintf(1, "ERROR: scontrol returned more hosts than expected (%d, %d)\n", i, num_hosts);
         goto done;
      }
      hostlist[i++] = s;
      sdprintf(3, "scontrol returned host %s (%d/%d)\n", s, i, num_hosts);
   }
   if (i != num_hosts) {
      sdprintf(1, "ERROR: expected %d hosts from scontrol.  Got %d\n",  num_hosts, i);
      goto done;
   }   
   
   hostlist[i] = NULL;
   
   ret = hostlist;
  done:
   if (scontrol_cmdline)
      free(scontrol_cmdline);
   if (f)
      pclose(f);
   if (!ret && hostlist) {
      for (i = 0; i < num_hosts; i++) {
         if (hostlist[i])
            free(hostlist[i]);
      }
      free(hostlist);
   }
   return ret;   
}

char **getHostAddrSinfo(unsigned int num_hosts, char **hostlist)
{
   const char *sinfo_path = SLURM_SINFO_BIN;
   const char *sinfo_args = "-O NodeAddr -h -n";
   const char *sinfo_suffix = "2> /dev/null";
   FILE *f = NULL;
   char **hostaddrlist = NULL, *s, *sinfo_cmdline = NULL, **ret = NULL;
   int i, j, hostnamelen;
   int result;
   size_t maxHostLen = 0, sinfo_cmdlineLen, len;

   hostaddrlist = calloc(num_hosts+1, sizeof(char*));

   for (i = 0; i < num_hosts; i++) {
       if (hostlist[i]) {
	   size_t thisLen = strlen(hostlist[i]);
	   if (thisLen > maxHostLen) maxHostLen = thisLen;
       }
   }

   sinfo_cmdlineLen = strlen(sinfo_path) + strlen(sinfo_args)
       + maxHostLen + strlen(sinfo_suffix) + 6;
   sinfo_cmdline = (char *) malloc(sinfo_cmdlineLen);

   for (i = 0; i < num_hosts; i++) {
      if (!hostlist[i] || !strlen(hostlist[i])) goto done;
      result = snprintf(sinfo_cmdline, sinfo_cmdlineLen, "%s %s \"%s\" %s",
			sinfo_path, sinfo_args, hostlist[i], sinfo_suffix);
      if (result >= sinfo_cmdlineLen) {
	 sdprintf(1, "ERROR: Formatting error creating sinfo cmdline '%s' (%d)\n",
		  sinfo_cmdline, result);
	 goto done;
      }
      sdprintf(2, "Running sinfo to get host address: %s\n", sinfo_cmdline);

      f = popen(sinfo_cmdline, "r");
      if (!f) {
	 sdprintf(1, "ERROR: Could not run sinfo: %s\n", sinfo_cmdline);
	 goto done;
      }

      len = 0;
      result = getline(&(hostaddrlist[i]), &len, f);
      pclose(f);
      if (result == -1) {
         int error = errno;
         sdprintf(1, "ERROR: Resolving '%s' failed: %s\n", hostlist[i], strerror(error));
         (void) error;
         goto done;
      }

      s = hostaddrlist[i];
      hostnamelen = strlen(s);
      for (j = 0; j < hostnamelen; j++) {
         if (!((s[j] >= '0' && s[j] <= '9') ||
               (s[j] >= 'a' && s[j] <= 'z') ||
               (s[j] >= 'A' && s[j] <= 'Z') ||
               (s[j] == '-' || s[j] == '_' || s[j] == '.'))) {
            s[j] = '\0';
            break;
         }
      }
      sdprintf(3, "sinfo returned hostaddr %s for %s\n", s, hostlist[i]);
   }
   if (i != num_hosts) {
      sdprintf(1, "ERROR: expected %d hosts from sinfo.  Got %d\n",  num_hosts, i);
      goto done;
   }

   ret = hostaddrlist;
  done:
   if (sinfo_cmdline)
      free(sinfo_cmdline);
   if (!ret && hostaddrlist) {
      for (i = 0; i < num_hosts; i++) free(hostaddrlist[i]);
      free(hostlist);
   }
   return ret;
}

int isFEHost(char **hostlist, unsigned int num_hosts)
{
   char host[256];
   char *last_host = NULL, *last_host_dot, *host_dot;
   unsigned int i;
   int result, error;
   int feresult = -1;
   
   for (i = 0; i < num_hosts; i++) {
      if (!last_host || strcmp(hostlist[i], last_host) > 0) {
         last_host = hostlist[i];
      }
   }
   sdprintf(2, "last_host = %s\n", last_host ? last_host : "(null)");
   if (!last_host) {
      error = errno;
      sdprintf(1, "ERROR: Could not get current system's hostname: %s\n", strerror(error));      
      goto done;
   }
   last_host = strdup(last_host);
   if (strcmp(last_host, "localhost") == 0) {
      feresult = 1;
      goto done;
   }
   result = gethostname(host, sizeof(host));
   if (result == -1) {
      error = errno;
      sdprintf(1, "ERROR: Could not get current system's hostname: %s\n", strerror(error));
      goto done;
   }
   host[sizeof(host)-1] = '\0';

   last_host_dot = strchr(last_host, '.');
   if (last_host_dot)
      *last_host_dot = '\0';
   host_dot = strchr(host, '.');
   if (host_dot)
      *host_dot = '\0';
   feresult = (strcmp(host, last_host) == 0) ? 1 : 0;
   if (feresult) {
      sdprintf(1, "Decided current host %s is FE\n", host);
   }
   else {
      sdprintf(1, "Decided current host %s is not FE, which is %s\n", host, last_host); 
   }
   
  done:
   if (last_host)
      free(last_host);
   (void) error;
   return feresult;      
}

static char* locSpecificDir(spindle_args_t *params) 
{
   char *dir = NULL, *expanded_dir = NULL, *realized_dir = NULL;
   
   dir = params->commpath;
   if (!dir) {
      sdprintf(1, "ERROR: Location not filled in\n");
      goto done;
   }
   expanded_dir = parse_location(dir, params->number);
   if (!expanded_dir) {
      sdprintf(1, "ERROR: Could not expand file-system dir %s\n", dir);
      goto done;
   }
   realized_dir = realize(expanded_dir);
   if (!realized_dir) {
      sdprintf(1, "ERROR: Could not turn dir to a real path\n");
   }

  done:
   if (expanded_dir)
      free(expanded_dir);

   return realized_dir;
}

#define SOCKET_PREFIX "spFE"

static char* exitSocketPath(spindle_args_t *params)
{
   char *realized_dir, *socket_path = NULL;
   char session_id_str[32];
   size_t socket_path_len;

   realized_dir = locSpecificDir(params);
   if (!realized_dir) {
      err_printf("Could not get real path for FE exit socket\n");
      goto done;
   }

   snprintf(session_id_str, sizeof(session_id_str), "%lu", (unsigned long) params->number);

   socket_path_len = strlen(realized_dir) + 1 + 
                     strlen(SOCKET_PREFIX) + 1 + 
                     strlen(session_id_str) + 1;

   socket_path = (char *) malloc(sizeof(char) * socket_path_len);
   snprintf(socket_path, socket_path_len, "%s/%s.%s", realized_dir, SOCKET_PREFIX, session_id_str);

  done:
   if (realized_dir)
      free(realized_dir);

   return socket_path;
}

static int createFEExitSocket(char *socket_path) 
{
   struct sockaddr_un local;
   int result, sock = -1, retval = -1;

   debug_printf("Creating unix socket for session at %s\n", socket_path);
   sock = socket(AF_UNIX, SOCK_STREAM, 0);
   if (sock == -1) {
      int error = errno;
      err_printf("Could not create socket for spindle session: %s\n", strerror(error));
      goto done;
   }

   local.sun_family = AF_UNIX;
   strncpy(local.sun_path, socket_path, sizeof(local.sun_path)-1);
   result = bind(sock, (struct sockaddr *) &local, sizeof(local));
   if (result == -1) {
      int error = errno;
      err_printf("Could not bind socket %s for spindle session: %s\n", local.sun_path, strerror(error));
      goto done;
   }

   result = listen(sock, 1);
   if (result == -1) {
      int error = errno;
      err_printf("Could not listen on server socket %s for spindle session: %s", local.sun_path, strerror(error));
      goto done;
   }
   retval = sock;
   
  done:
   if (sock != -1 && result == -1)
      close(sock); 

   return retval;
}

int doesFEExitSocketExist(spindle_args_t *params) 
{
   struct stat sb;
   char * socket_path = NULL;
   
   socket_path = exitSocketPath(params);
   if (!socket_path)
      return 0;

   if (stat(socket_path, &sb) == -1)
      return 0;

   return S_ISSOCK(sb.st_mode);
}

int waitForSpankSessionEnd(spindle_args_t *params) 
{
   int fd = -1, sockfd = -1, error, retval = -1, result;
   char msg;
   char *socket_path = NULL;

   sdprintf(2, "In waitForSpankSessionEnd\n");

   socket_path = exitSocketPath(params);
   if (!socket_path) 
      goto done;

   sockfd = createFEExitSocket(socket_path);
   if (sockfd == -1) 
      goto done;
   
   fd = accept(sockfd, NULL, NULL);
   if (fd == -1) {
      error = errno;
      err_printf("Could not accept session exit socket connection: %s\n", strerror(error));
      goto done;
   }

   do {
      result = read(fd, &msg, 1);
   } while (result == -1 && errno == EINTR);
   if (result == -1) {
      error = errno;
      err_printf("Failed to read from session exit socket: %s\n", strerror(error));
      goto done;
   }
   if (msg != 'q') {
      error = errno;
      err_printf("Recieved incorrect msg character: %c\n", msg);
      goto done;
   }
   
   sdprintf(2, "Received session exit message\n");   
   retval = 0;

  done:
   if (fd != -1)
      close(fd);
   if (sockfd != -1)
      close(sockfd);
   if (socket_path) {
      unlink(socket_path);
      free(socket_path);
   }
   return retval;
}

int signalSpankSessionEnd(spindle_args_t *params) 
{
   int sockfd = -1, error, retval = -1, result;
   char msg;
   struct sockaddr_un saddr;
   char *socket_path = NULL;

   sdprintf(2, "In signalSpankSessionEnd\n");

   socket_path = exitSocketPath(params);
   if (!socket_path) 
      goto done;

   sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (sockfd == -1) {
      error = errno;
      err_printf("Could not create session exit socket: %s\n", strerror(error));
      goto done;
   }

   saddr.sun_family = AF_UNIX;
   strncpy(saddr.sun_path, socket_path, sizeof(saddr.sun_path)-1);

   result = connect(sockfd, (const struct sockaddr *) &saddr, sizeof(saddr));
   if (result == -1) {
      int error = errno;
      err_printf("Failed to connect to session exit socket: %s\n", strerror(error));
      return -1;
   }

   msg = 'q';
   do {
      result = write(sockfd, &msg, 1);
   } while (result == -1 && errno == EINTR);
   if (result == -1) {
      error = errno;
      err_printf("Failed to write value to session exit socket: %s\n", strerror(error));
      goto done;
   }
   if (result == 0) {
      err_printf("Socket closed before we could write to session exit socket\n");
      goto done;
   }

   retval = 0;
   
  done:
   if (sockfd != -1)
      close(sockfd);
   if (socket_path) 
      free(socket_path);

   return retval;
}

char *unique_file = NULL;

void cleanup_unique_file()
{
   if (unique_file) {
      unlink(unique_file);
      free(unique_file);
      unique_file = NULL;
   }
}

#define UNIQUE_FILE_NAME "spindle_unique"

int isBEProc(spindle_args_t *params, unsigned int exit_phase)
{
   char *realized_dir = NULL, *phase_name = NULL;
   char hostname[256], session_id_str[32];
   size_t unique_file_len;
   int beproc_result = -1;
   int fd = -1, error;
   
   realized_dir = locSpecificDir(params);

   gethostname(hostname, sizeof(hostname));
   hostname[sizeof(hostname)-1] = '\0';

   snprintf(session_id_str, sizeof(session_id_str), "%lu", (unsigned long) params->number);

   phase_name = exit_phase ? "exit" : "launch";

   unique_file_len = strlen(realized_dir) + 1 +
      strlen(UNIQUE_FILE_NAME) + 1 +
      strlen(phase_name) + 1 +
      strlen(hostname) + 1 +
      strlen(session_id_str) + 1;

   unique_file = (char *) malloc(sizeof(char*) * unique_file_len);
   snprintf(unique_file, unique_file_len, "%s/%s.%s.%s.%s", realized_dir, UNIQUE_FILE_NAME, phase_name, hostname, session_id_str);

   spindle_mkdir(realized_dir);

   fd = open(unique_file, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
   error = errno;
   sdprintf(2, "Opened %s to result %d\n", unique_file, fd);
   if (fd != -1)
      beproc_result = 1;
   else if (error == EEXIST) {
      beproc_result = 0;
      free(unique_file);
      unique_file = NULL;
   } else {
      sdprintf(1, "ERROR: Could not create spindle unique_file %s: %s\n", unique_file, strerror(error));
      goto done;
   }
   
  done:
   if (realized_dir)
      free(realized_dir);
   if (fd != -1)
      close(fd);
   sdprintf(2, "returning %d\n", beproc_result);
   return beproc_result;
}

char *encodeCmdArgs(int sargc, char **sargv)
{
   int i, pos;
   size_t len = 0;
   char *param;
   int first = 1;
   char *str;
   
   if (!sargc || !sargv || !sargv[0])
      return NULL;

   for (i = 0; i < sargc; i++) {
      param = sargv[i];
      if (!param || !param[0])
         continue;

      if (!first)
         len++;
      first = 0;
      
      len += strlen(param);
   }
   if (!len)
      return NULL;
   len++;

   str = (char *) malloc(len);
   pos = i = 0;
   first = 1;
   for (i = 0; i < sargc; i++) {
      param = sargv[i];
      if (!param || !param[0])
         continue;

      if (!first)
         str[pos++] = ' ';
      first = 0;
      
      strncpy(str + pos, sargv[i], len - pos);
      pos += strlen(sargv[i]);
   }

   return str;
}

static int safe_read(int fd, void *buf, size_t count)
{
   size_t bytes_read = 0;
   int result;
   
   while (bytes_read < count) {
      result = read(fd, ((char *) buf)+bytes_read, count-bytes_read);
      if (result == -1 && errno == EINTR)
         continue;
      else if (result == -1)
         return -1;
      else if (result == 0)
         return bytes_read;
      bytes_read += result;
   }
   return bytes_read;
}

static int safe_write(int fd, void *buf, size_t count)
{
   size_t bytes_written = 0;
   int result;

   while (bytes_written < count) {
      result = write(fd, ((char *) buf) + bytes_written, count - bytes_written);
      if (result == -1 && errno == EINTR)
         continue;
      else if (result == -1)
         return -1;
      else if (result == 0)
         return bytes_written;
      bytes_written += result;
   }
   return bytes_written;
}

static int read_output(int fd, size_t *output_len, char **output_str)
{
   int result;
   size_t len;
   char *output;
   
   result = safe_read(fd, &len, sizeof(len));
   if (result != sizeof(len))
      return -1;
   if (len >= 32*1024)
      return -1;
   output = (char *) malloc(len+1);
   result = safe_read(fd, output, len);
   output[len] = '\0';
   if (result != len) {
      free(output);
      return -1;
   }
   *output_len = len;
   *output_str = output;
   return 0;
}

void push_env(spank_t spank, saved_env_t **env)
{
   saved_env_t *e = (saved_env_t *) malloc(sizeof(saved_env_t));
   
   e->new_home = readSpankEnv(spank, "HOME");
   e->old_home = getenv("HOME");
   e->new_path = readSpankEnv(spank, "PATH");
   e->old_path = getenv("PATH");
   e->new_pwd = readSpankEnv(spank, "PWD");
   e->old_pwd = getenv("PWD");
   e->new_tmpdir = readSpankEnv(spank, "TMPDIR");
   e->old_tmpdir = getenv("TMPDIR");
   e->new_spindledebug = readSpankEnv(spank, "SPINDLE_DEBUG");
   e->old_spindledebug = getenv("SPINDLE_DEBUG");

   if (e->new_pwd) {
      if (chdir(e->new_pwd) == -1)
         sdprintf(1, "WARNING: Could not chdir to %s: %s\n", e->new_pwd, strerror(errno));
   }

   if (e->new_home)
      setenv("HOME", e->new_home, 1);
   if (e->new_path)
      setenv("PATH", e->new_path, 1);
   if (e->new_pwd)
      setenv("PWD", e->new_pwd, 1);
   if (e->new_tmpdir)
      setenv("TMPDIR", e->new_tmpdir, 1);
   if (e->new_spindledebug)
      setenv("SPINDLE_DEBUG", e->new_spindledebug, 1);

   *env = e;
}

void pop_env(saved_env_t *env)
{
   if (env->old_home)
      setenv("HOME", env->old_home, 1);
   else
      unsetenv("HOME");
   if (env->old_path)
      setenv("PATH", env->old_path, 1);
   else
      unsetenv("PATH");
   if (env->old_pwd)
      setenv("PWD", env->old_pwd, 1);
   else
      unsetenv("PWD");
   if (env->old_tmpdir)
      setenv("TMPDIR", env->old_tmpdir, 1);
   else
      unsetenv("TMPDIR");
   if (env->old_spindledebug)
      setenv("SPINDLE_DEBUG", env->old_spindledebug, 1);
   else
      unsetenv("SPINDLE_DEBUG");

   if (env->old_pwd) {
      if (chdir(env->old_pwd) == -1)
         sdprintf(1, "WARNING: Could not chdir to %s: %s\n", env->old_pwd, strerror(errno));
   }

   if (env->new_home)
      free(env->new_home);
   if (env->new_path)
      free(env->new_path);
   if (env->new_pwd)
      free(env->new_pwd);
   if (env->new_tmpdir)
      free(env->new_tmpdir);
   if (env->new_spindledebug)
      free(env->new_spindledebug);
   free(env);
}

int dropPrivilegeAndRun(dpr_function_t func, uid_t uid, void *input, char **output_str)
{
   int pipe_fds[2];
   int result, error, read_result, child_result, status;
   pid_t pid;
   size_t output_len;
   char *child_output_str = NULL;
   struct passwd *userinfo;

   result = pipe(pipe_fds);
   if (result == -1) {
      error = errno;
      fprintf(stderr, "Spindle Error. Pipe failed: %s\n", strerror(error));
      return -1;
   }

   pid = fork();
   if (pid == -1) {
      error = errno;
      fprintf(stderr, "Spindle Error.  Fork failed: %s\n", strerror(error));
      return -1;
   }
   else if (!pid) {
      close(pipe_fds[0]);

      if (getuid() == 0) {
         errno = 0;
         userinfo = getpwuid(uid);
         if (!userinfo) {
            error = errno;
            fprintf(stderr, "Spindle Error.  Could not look up passwd info for uid %d: %s\n", uid, strerror(error));
            exit(-1);
         }
         
         result = setgid(userinfo->pw_gid);
         if (result == -1) {
            error = errno;
            fprintf(stderr, "Spindle Error. Could not drop group privileges to gid %d: %s\n", userinfo->pw_gid, strerror(error));
            exit(-1);
         }
         result = setuid(uid);
         if (result == -1) {
            error = errno;
            fprintf(stderr, "Spindle Error. Could not drop privileges to uid %d: %s\n", userinfo->pw_gid, strerror(error));
            exit(-1);
         }
      }

      if (getuid() == 0) {
         fprintf(stderr, "Spindle Error.  Could not drop root privileges\n");
         exit(-1);
      }

      child_output_str = NULL;
      result = func(input, &child_output_str);
      if (result != 0) {
         close(pipe_fds[1]);
         exit(-1);
      }

      output_len = child_output_str ? strlen(child_output_str) : 0;
      if (output_len > 32*1024) {
         fprintf(stderr, "Spindle Error.  Internal error, excessive buffer length of %lu\n", output_len);
         exit(-1);
      }
      result = safe_write(pipe_fds[1], &output_len, sizeof(output_len));
      if (result != sizeof(output_len)) {
         error = errno;
         fprintf(stderr, "Spindle error.  Could not write strlen to pipe: %s\n", strerror(error));
         exit(-1);
      }
      if (output_len) {
         result = safe_write(pipe_fds[1], child_output_str, output_len+1);
         if (result != output_len+1) {
            error = errno;
            fprintf(stderr, "Spindle error.  Could not write result string to pipe: %s\n", strerror(error));
            exit(-1);
         }
      }
      close(pipe_fds[1]);
      exit(0);
   }
   else {
      close(pipe_fds[1]);
      read_result = read_output(pipe_fds[0], &output_len, &child_output_str);
      close(pipe_fds[0]);

      result = waitpid(pid, &status, 0);
      if (result == -1)
         child_result = -1;
      else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
         child_result = -1;
      else
         child_result = 0;
      
      if (read_result == -1 || child_result == -1) {
         if (child_output_str)
            free(child_output_str);
         return -1;
      }
      
      *output_str = child_output_str;
      return 0;
   }
}

int registerFEPid(pid_t pid, spindle_args_t *args)
{
   char pid_file[4097];
   char pid_s[32];
   int fd;
   int result;

   snprintf(pid_file, sizeof(pid_file), "%s/fepid", args->commpath);
   pid_file[sizeof(pid_file)-1] = '\0';

   snprintf(pid_s, sizeof(pid_s), "%d\n", (int) pid);
   pid_s[sizeof(pid_s)-1] = '\0';
   
   sdprintf(2, "Creating file containing FE pid %d at %s\n", (int) pid, pid_file);

   fd = open(pid_file, O_WRONLY | O_CREAT, 0600);
   if (fd == -1) {
      sdprintf(1, "ERROR: Could not create fe pid file %s\n", pid_file);
      return -1;
   }

   result = safe_write(fd, pid_s, strlen(pid_s));
   close(fd);
   if (result == -1) {
      sdprintf(1, "ERROR: Could not write to pid file %s\n", pid_file);
      return -1;
   }

   return 0;
}

int readFEPid(pid_t *pid, spindle_args_t *args)
{
   char pid_file[4097];
   char pid_s[32];
   pid_t pid_result;
   int fd, result;

   snprintf(pid_file, sizeof(pid_file), "%s/fepid", args->commpath);
   pid_file[sizeof(pid_file)-1] = '\0';

   sdprintf(2, "Reading FE pid from %s\n", pid_file);

   fd = open(pid_file, O_RDONLY);
   if (fd == -1) {
      sdprintf(1, "Could not open fe pid file %s\n", pid_file);
      *pid = 0;
      return 0;
   }

   result = safe_read(fd, pid_s, sizeof(pid_s));
   close(fd);
   if (result == -1) {
      sdprintf(1, "ERROR: Could not read from pid file %s\n", pid_file);
      return -1;
   }

   pid_result = (pid_t) atoi(pid_s);
   if (pid_result <= 0) {
      sdprintf(1, "ERROR: Read invalid pid %d from fe pid file %s\n", pid_result, pid_file);
      return -1;
   }
   
   sdprintf(2, "Found FE pid to be %d\n", (int) pid_result);
   
   *pid = pid_result;
   return 0;   
}

void decodeCmdArgs(char *cmd, int *sargc, char ***sargv)
{
   int i, len, t = 0;
   int num_spaces = 0, argc = 0;
   char *target = NULL, **argv = NULL;
   char c;

   len = strlen(cmd);
   if (!len) {
      goto done;
   }
      
   for (i = 0; i < len; i++)
      if (cmd[i] == ' ')
         num_spaces++;
   target = malloc(strlen(cmd)+1);
   argv = (char **) calloc(num_spaces+2, sizeof(char*));
   
   for (i = 0; i < len; i++) {
      if (cmd[i] == '\\' && i+1 < len) {
         switch(cmd[++i]) {
            case 'a': c = '\a'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'v': c = '\v'; break;
            case '0': c = '\0'; break;
            default: c = cmd[i];
         }
      }
      else if (cmd[i] == ' ') {
         if (!t)
            continue;
         target[t] = '\0';
         argv[argc++] = strdup(target);
         t = 0;
         continue;
      }
      else {
         c = cmd[i];
      }
      target[t++] = c;      
   }

   if (!argc) {
      goto done;
   }
   argv[argc] = NULL;

  done:

   if (target)
      free(target);
   if (!argc && argv) {
      free(argv);
      argv = NULL;
   }
   *sargc = argc;
   *sargv = argv;
}

pid_t grandchild_fork()
{
   int pipe_fds[2], status;
   pid_t child_pid = -1;
   pid_t grandchild_pid = -1;
   int result, fork_result = -1;

   pipe_fds[0] = pipe_fds[1] = -1;
   result = pipe(pipe_fds);
   if (result == -1) {
      sdprintf(1, "ERROR: pipe() failed in grandchild_fork.  Aborting spindle\n");
      return -1;
   }

   child_pid = fork();
   if (child_pid == -1) {
      sdprintf(1, "ERROR: Could not fork process.  Aborting spindle\n");
      goto done;
   }
   if (child_pid) {
      result = safe_read(pipe_fds[0], &grandchild_pid, sizeof(grandchild_pid));
      if (result != sizeof(grandchild_pid)) {
         sdprintf(1, "ERROR during grandchild fork.  Aborting spindle\n");
         goto done;
      }

      result = waitpid(child_pid, &status, 0);
      if (result == -1) {
         sdprintf(1, "ERROR collecting pid after fork.  Aborting spindle\n");
         goto done;
      }
      if (!WIFEXITED(status) && WEXITSTATUS(status) != 0) {
         sdprintf(1, "ERROR with invalid child exit during grandchild fork.  Aborting spindle\n");
         goto done;
      }

      fork_result = grandchild_pid;
      goto done;
   }

   //In child
   grandchild_pid = fork();
   if (grandchild_pid == -1) {
      sdprintf(1, "ERROR: Could not fork process. Aborting spindle\n");
      goto done;
   }
   else if (grandchild_pid) {
      result = safe_write(pipe_fds[1], &grandchild_pid, sizeof(grandchild_pid));
      close(pipe_fds[0]);
      close(pipe_fds[1]);
      exit(result == sizeof(grandchild_pid) ? 0 : -1);
   }
   //In grandchild
   setpgid(0, 0); /* escape to own process group */
   fork_result = 0;
   goto done;
   
  done:
   if (pipe_fds[0] != -1)
      close(pipe_fds[0]);
   if (pipe_fds[1] != -1)
      close(pipe_fds[1]);
   return fork_result;
}


typedef struct
{
   char **hosts;
   size_t size;
   size_t cur;
} host_list_t;

static void addHost(char *host, host_list_t *hlist)
{
   if (!hlist->hosts) {
      if (!hlist->size)
         hlist->size = 8;
      else
         hlist->size++;
      hlist->hosts = (char **) malloc(hlist->size * sizeof(char*));
      hlist->cur = 0;
   }
   if (hlist->cur + 2 >= hlist->size) {
      hlist->size *= 2;
      hlist->hosts = (char **) realloc(hlist->hosts, hlist->size*sizeof(char*));
   }
   hlist->hosts[hlist->cur++] = host;
   hlist->hosts[hlist->cur] = NULL;
}

static void freeHosts(char **hosts)
{
   char **h;
   if (!hosts)
      return;
   for (h = hosts; *h; h++) {
      free(*h);
   }
   free(hosts);
}

static char *expandNumberRange(char *range, unsigned int *start, unsigned int *end)
{
   char *first_number, *second_number, *next, *pos;

   first_number = range;
   pos = first_number;

   if (*first_number < '0' || *first_number > '9')
      return NULL;
   while (*pos >= '0' && *pos <= '9') pos++;

   if (*pos == ',') {
      second_number = NULL;
      next = pos+1;
   }
   else if (*pos == ']') {
      second_number = NULL;
      next = pos+1;
   }
   else if (*pos == '-') {
      second_number = pos+1;
      pos++;
      if (*second_number < '0' || *second_number > '9')
         return NULL;
      while (*pos >= '0' && *pos <= '9') pos++;
      next = pos+1;
   }
   else
      return NULL;

   *start = atoi(first_number);
   *end = second_number ? atoi(second_number) : *start;
   if (*end < *start)
      return NULL;
   return next;
}

static int charLengthOfNumber(int num)
{
   int i = num, len = 0;
   while (i) {
      i /= 10;
      len++;
   }
   return len;
}

static int expandOneShortHost(char *start, host_list_t *hlist)
{
   char *open_bracket, *close_bracket, *prefix = NULL, *suffix = NULL;
   char *pos, *host;
   size_t prefix_len = 0, suffix_len = 0, shorthost_len, number_len;
   unsigned int number_start, number_end, i;
   int expand_result = -1;

   open_bracket = strchr(start, '[');
   if (!open_bracket) {
      addHost(strdup(start), hlist);
      expand_result = 0;
      goto done;
   }
   close_bracket = strchr(open_bracket, ']');
   if (!close_bracket)
      goto done;

   shorthost_len = strlen(start);

   prefix_len = open_bracket - start;
   prefix = (char *) malloc(prefix_len + 1);
   strncpy(prefix, start, prefix_len);
   prefix[prefix_len] = '\0';

   suffix_len = (start + shorthost_len) - close_bracket;
   suffix = (char *) malloc(suffix_len + 1);
   memcpy(suffix, close_bracket+1, suffix_len);
   suffix[suffix_len] = '\0';

   pos = open_bracket + 1;
   do {
      pos = expandNumberRange(pos, &number_start, &number_end);
      if (!pos)
         goto done;
      for (i = number_start; i <= number_end; i++) {
         number_len = charLengthOfNumber(i);
         host = (char *) malloc(prefix_len + number_len + suffix_len + 1);
         snprintf(host, prefix_len + number_len + suffix_len + 1, "%s%u%s", prefix, i, suffix);
         if (strchr(host, '[')) {
            expandOneShortHost(host, hlist);
            free(host);
         }
         else
            addHost(host, hlist);
      }
   } while (pos < close_bracket);

   expand_result = 0;
  done:
   if (prefix)
      free(prefix);
   if (suffix)
      free(suffix);

   return expand_result;
}

char **getHostsParse(unsigned int num_hosts, const char *shortlist)
{
   char *list, *pos, *start;
   int in_bracket = 0, result;
   size_t len;
   host_list_t hlist;

   hlist.hosts = NULL;
   hlist.cur = 0;
   hlist.size = num_hosts;

   list = strdup(shortlist);
   len = strlen(list);;

   start = list;
   for (pos = list; pos <= list+len; pos++) {
      if (*pos == '[') in_bracket++;
      if (*pos == ']') in_bracket--;

      if (!in_bracket && (*pos == ',' || *pos == '\0')) {
         *pos = '\0';
         result = expandOneShortHost(start, &hlist);
         if (result == -1) {
            freeHosts(hlist.hosts);
            return NULL;
         }
         start = pos+1;
      }
   }

   free(list);
   return hlist.hosts;
}

int signalFE(pid_t fepid)
{
   kill(fepid, SIGUSR1);
   return 0;
}

int prepWaitForSignalFE()
{
   sigset_t usr1set;
   int result, error;

   sigemptyset(&usr1set);
   sigaddset(&usr1set, SIGUSR1);
   
   result = sigprocmask(SIG_BLOCK, &usr1set, NULL);
   if (result == -1) {
      error = errno;
      sdprintf(1, "ERROR: Could not block signals in FE: %s\n", strerror(error));
      return -1;
   }
   return 0;
}

int waitForSignalFE()
{
   sigset_t usr1set;
   int result;
   siginfo_t info;

   sigemptyset(&usr1set);
   sigaddset(&usr1set, SIGUSR1);

   do {
      result = sigwaitinfo(&usr1set, &info);
   } while (result == -1 && errno == EINTR);
   
   return 0;
}

char *readSpankEnv(spank_t spank, const char *envname)
{
   char *buffer;
   int buffer_size = 4096;
   spank_err_t err;
   spank_context_t ctx;

   /* spank_getenv crashes if called from S_CTX_JOB_SCRIPT */
   ctx = spank_context();
   if (ctx == S_CTX_JOB_SCRIPT) {
      buffer = getenv(envname);
      return buffer ? strdup(buffer) : NULL;
   }

   buffer = (char *) malloc(buffer_size);
   for (;;) {
      err = spank_getenv(spank, envname, buffer, buffer_size);
      if (err != ESPANK_NOSPACE)
         break;
      buffer_size *= 2;
      free(buffer);
      buffer = (char *) malloc(buffer_size);
   }
   if (err == ESPANK_ENV_NOEXIST) {
      free(buffer);
      buffer = getenv(envname);
      return buffer ? strdup(buffer) : NULL;
   }
   if (err != ESPANK_SUCCESS) {
      free(buffer);
      return NULL;
   }
   return buffer;
}


int superclose()
{
   int i;

   for (i = 0; i < 255; i++)
      close(i);
   open("/dev/null", O_RDONLY);
   open("/dev/null", O_WRONLY);
   open("/dev/null", O_WRONLY);   
   return 0;
}
