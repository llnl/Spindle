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

#include "config.h"
#include "config_mgr.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <limits.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wordexp.h>
#include <sys/wait.h>
#include <fcntl.h>

extern "C" {
#include "keyfile.h"
}

struct MountPoint {
   std::string path;
   std::string fstype;
   bool is_remote;
   uint64_t avail_gb;

   MountPoint(const std::string& p, const std::string& fs)
      : path(p), fstype(fs), is_remote(false), avail_gb(0) {}
};

static bool is_remote_filesystem(const std::string& fstype)
{
   static const char* remote_types[] = {
      "nfs", "nfs4", "lustre", "gpfs", "panfs",
      "ceph", "glusterfs", "afs", "smbfs", "cifs",
      nullptr
   };

   for (int i = 0; remote_types[i]; i++) {
      if (fstype == remote_types[i] ||
          fstype.find(std::string("fuse.") + remote_types[i]) == 0) {
         return true;
      }
   }
   return false;
}

static bool is_kernel_filesystem(const std::string& path)
{
   // Hardcode known kernel filesystem paths
   // Note: /dev/shm is NOT a kernel filesystem, it's tmpfs
   return path == "/proc" || path == "/sys" ||
          (path.find("/proc/") == 0) ||
          (path.find("/sys/") == 0) ||
          (path == "/dev" || (path.find("/dev/") == 0 && path != "/dev/shm"));
}

static bool should_ignore_mount(const std::string& path, const std::string& fstype)
{
   // Ignore virtual/system filesystems
   if (fstype == "proc" || fstype == "sysfs" || fstype == "devtmpfs" ||
       fstype == "devpts" || fstype == "securityfs" || fstype == "pstore" ||
       fstype == "autofs" || fstype == "mqueue" || fstype == "hugetlbfs" ||
       fstype.find("cgroup") == 0 || fstype.find("bpf") == 0) {
      return true;
   }

   // Ignore system tmpfs mounts (but NOT /dev/shm which is useful)
   if (fstype == "tmpfs" && (path == "/run" || path == "/sys/fs/cgroup" ||
                              path.find("/run/") == 0 || path.find("/sys/") == 0 ||
                              (path.find("/dev/") == 0 && path != "/dev/shm"))) {
      return true;
   }

   return false;
}

static uint64_t get_available_gb(const std::string& path)
{
   struct statvfs stat;
   if (statvfs(path.c_str(), &stat) != 0) {
      return 0;
   }

   uint64_t avail_bytes = (uint64_t)stat.f_bavail * stat.f_frsize;
   return avail_bytes / (1024ULL * 1024 * 1024);
}

static std::vector<MountPoint> parse_mounts()
{
   std::vector<MountPoint> mounts;
   std::ifstream mounts_file("/proc/mounts");
   std::string line;
   std::set<std::string> seen_paths;

   while (std::getline(mounts_file, line)) {
      std::istringstream iss(line);
      std::string device, path, fstype, options;

      if (!(iss >> device >> path >> fstype >> options)) {
         continue;
      }

      if (should_ignore_mount(path, fstype)) {
         continue;
      }

      // Skip duplicate paths
      if (seen_paths.count(path)) {
         continue;
      }
      seen_paths.insert(path);

      MountPoint mp(path, fstype);

      // Overlay filesystems are typically local (container backing storage)
      if (fstype == "overlay") {
         mp.is_remote = false;
      } else {
         mp.is_remote = is_remote_filesystem(fstype);
      }

      if (!mp.is_remote) {
         mp.avail_gb = get_available_gb(path);

         // Skip local mounts with < 1GB available
         if (mp.avail_gb < 1) {
            continue;
         }

         // Check if writable
         if (access(path.c_str(), W_OK) != 0) {
            continue;
         }
      }

      mounts.push_back(mp);
   }

   // Sort by path length (longest first) for prefix matching
   std::sort(mounts.begin(), mounts.end(),
             [](const MountPoint& a, const MountPoint& b) {
                return a.path.length() > b.path.length();
             });

   return mounts;
}

enum ExpandResult {
   EXPAND_OK,
   EXPAND_UNDEFINED_VAR,
   EXPAND_EMPTY,
   EXPAND_ERROR
};

struct ExpandedPath {
   std::string expanded;
   ExpandResult result;
};

static ExpandedPath expand_path(const std::string& path)
{
   ExpandedPath ep;

   if (path.empty()) {
      ep.expanded = path;
      ep.result = EXPAND_EMPTY;
      return ep;
   }

   // Use wordexp for shell expansion (handles $VAR, ~, etc.)
   wordexp_t exp_result;
   int ret = wordexp(path.c_str(), &exp_result, WRDE_NOCMD | WRDE_UNDEF);

   if (ret == WRDE_BADVAL) {
      // Undefined variable
      ep.expanded = "";
      ep.result = EXPAND_UNDEFINED_VAR;
      return ep;
   } else if (ret != 0) {
      // Other expansion error
      ep.expanded = "";
      ep.result = EXPAND_ERROR;
      return ep;
   }

   if (exp_result.we_wordc > 0 && exp_result.we_wordv[0] && exp_result.we_wordv[0][0] != '\0') {
      ep.expanded = exp_result.we_wordv[0];
      ep.result = EXPAND_OK;
   } else {
      // Empty expansion
      ep.expanded = "";
      ep.result = EXPAND_EMPTY;
   }

   wordfree(&exp_result);
   return ep;
}

static std::string resolve_real_path(const std::string& path)
{
   if (path.empty()) {
      return path;
   }

   char resolved[PATH_MAX];
   if (realpath(path.c_str(), resolved) != nullptr) {
      return std::string(resolved);
   }

   // If realpath fails, try to handle relative paths manually
   if (path[0] != '/') {
      char cwd[PATH_MAX];
      if (getcwd(cwd, sizeof(cwd))) {
         std::string abs = std::string(cwd) + "/" + path;
         if (realpath(abs.c_str(), resolved) != nullptr) {
            return std::string(resolved);
         }
         return abs;  // Return constructed absolute path even if realpath fails
      }
   }

   return path;  // Return original if we can't resolve
}

static const MountPoint* find_mount_for_path(const std::string& resolved_path,
                                               const std::vector<MountPoint>& mounts)
{
   if (resolved_path.empty()) {
      return nullptr;
   }

   // Find longest matching mount point
   for (const auto& mp : mounts) {
      if (resolved_path == mp.path ||
          (resolved_path.find(mp.path + "/") == 0)) {
         return &mp;
      }
   }

   return nullptr;
}

struct PathAnalysis {
   std::string original;
   std::string expanded;
   std::string resolved;
   bool is_ok;
   std::vector<std::string> warnings;
   const MountPoint* mount;

   PathAnalysis(const std::string& orig) : original(orig), is_ok(false), mount(nullptr) {}
};

static std::string find_writable_parent(const std::string& path)
{
   // Find the first existing parent directory
   std::string test_path = path;

   while (!test_path.empty() && test_path != "/") {
      if (access(test_path.c_str(), F_OK) == 0) {
         // Path exists, check if writable
         return test_path;
      }

      // Move to parent
      size_t last_slash = test_path.rfind('/');
      if (last_slash == std::string::npos) {
         break;
      }
      test_path = test_path.substr(0, last_slash);
      if (test_path.empty()) {
         test_path = "/";
      }
   }

   return "/";
}

static PathAnalysis analyze_path(const std::string& path, const std::vector<MountPoint>& mounts,
                                   bool check_for_python = false)
{
   PathAnalysis analysis(path);

   if (path.empty()) {
      analysis.warnings.push_back("Path is empty");
      return analysis;
   }

   // Expand variables
   ExpandedPath ep = expand_path(path);
   analysis.expanded = ep.expanded;

   if (ep.result == EXPAND_UNDEFINED_VAR) {
      analysis.warnings.push_back("Variable is not set");
      return analysis;
   } else if (ep.result == EXPAND_EMPTY) {
      analysis.warnings.push_back("Variable expands to empty string");
      return analysis;
   } else if (ep.result == EXPAND_ERROR) {
      analysis.warnings.push_back("Cannot expand path");
      return analysis;
   }

   // Check for empty segments (e.g., /var// from /var/$EMPTY/)
   if (analysis.expanded.find("//") != std::string::npos) {
      analysis.warnings.push_back("Path contains empty segment (variable may be empty)");
   }

   // Resolve to real path
   analysis.resolved = resolve_real_path(ep.expanded);

   // Check for kernel filesystems first (these are always OK)
   if (is_kernel_filesystem(analysis.resolved)) {
      analysis.is_ok = true;
      return analysis;
   }

   // Check if path exists
   bool path_exists = false;
   struct stat st;
   if (stat(analysis.resolved.c_str(), &st) == 0) {
      path_exists = true;
   } else {
      analysis.warnings.push_back("Path does not exist");

      // For non-python paths, check if it will be created
      if (!check_for_python) {
         analysis.warnings[analysis.warnings.size()-1] += " (will be created by Spindle)";
         // Check if parent is writable
         std::string parent = find_writable_parent(analysis.resolved);
         if (access(parent.c_str(), W_OK) != 0) {
            analysis.warnings.push_back("Parent directory (" + parent + ") is not writable");
            return analysis;
         }
      } else {
         // Python prefix must exist
         return analysis;
      }
   }

   // Special validation for Python prefix
   if (check_for_python && path_exists) {
      // Check for bin/python or bin/python3
      std::string python_path = analysis.resolved + "/bin/python";
      std::string python3_path = analysis.resolved + "/bin/python3";

      bool has_python = (access(python_path.c_str(), X_OK) == 0);
      bool has_python3 = (access(python3_path.c_str(), X_OK) == 0);

      if (!has_python && !has_python3) {
         analysis.warnings.push_back("No bin/python or bin/python3 found (not a Python installation?)");
      }
   }

   // Find mount point (only for cache/comm paths, not python prefix)
   if (!check_for_python) {
      analysis.mount = find_mount_for_path(analysis.resolved, mounts);

      if (!analysis.mount) {
         analysis.warnings.push_back("Cannot determine filesystem type");
         return analysis;
      }

      if (analysis.mount->is_remote) {
         analysis.warnings.push_back("Path is on REMOTE filesystem (" + analysis.mount->fstype + ")");
         return analysis;
      }
   }

   // All checks passed (or acceptable warnings)
   if (analysis.warnings.empty()) {
      analysis.is_ok = true;
   }

   return analysis;
}

static void print_filesystem_recommendations(const std::vector<MountPoint>& mounts,
                                              const ConfigMap& confmap)
{
   std::cout << "# Spindle filesystem recommendations:" << std::endl;

   // Show local writable mounts
   std::vector<const MountPoint*> local_mounts;
   for (const auto& mp : mounts) {
      if (!mp.is_remote) {
         local_mounts.push_back(&mp);
      }
   }

   if (!local_mounts.empty()) {
      std::cout << "#   Local: ";
      bool first = true;
      for (const auto* mp : local_mounts) {
         if (!first) std::cout << ", ";
         std::cout << mp->path << " (" << mp->fstype << ", "
                   << mp->avail_gb << "GB)";
         first = false;
      }
      std::cout << std::endl;
   } else {
      std::cout << "#   WARNING: No suitable local filesystems found!" << std::endl;
   }

   std::cout << "#" << std::endl;

   // Analyze each configured option
   auto analyze_option = [&](SpindleConfigID config_id, const char* name, bool check_python = false) {
      if (!confmap.isSet(config_id)) {
         return;
      }

      auto result = confmap.getValueString(config_id);
      if (!result.first || result.second.empty()) {
         return;
      }

      std::string paths = result.second;
      std::vector<PathAnalysis> analyses;

      // Split on colons for multi-path options
      size_t pos = 0;
      while (pos < paths.length()) {
         size_t end = paths.find(':', pos);
         if (end == std::string::npos) {
            end = paths.length();
         }

         std::string path = paths.substr(pos, end - pos);
         if (!path.empty()) {
            analyses.push_back(analyze_path(path, mounts, check_python));
         }

         pos = end + 1;
      }

      if (analyses.empty()) {
         return;
      }

      std::cout << "#   " << name << " analysis:" << std::endl;

      for (const auto& analysis : analyses) {
         const char* status = analysis.is_ok ? "OK" : "WARNING";
         std::cout << "#     " << status << ": " << analysis.original;

         if (!analysis.resolved.empty()) {
            std::cout << " -> " << analysis.resolved;
            if (analysis.mount && !analysis.mount->is_remote) {
               std::cout << " (" << analysis.mount->fstype << ", "
                        << analysis.mount->avail_gb << "GB)";
            } else if (analysis.mount && analysis.mount->is_remote) {
               std::cout << " (" << analysis.mount->fstype << ")";
            }
         } else if (!analysis.expanded.empty()) {
            std::cout << " -> " << analysis.expanded;
         } else {
            std::cout << " -> (empty)";
         }

         std::cout << std::endl;

         for (const auto& warning : analysis.warnings) {
            std::cout << "#       " << warning << std::endl;
         }
      }

      std::cout << "#" << std::endl;
   };

   analyze_option(confCommPaths, "commpaths");
   analyze_option(confCachePaths, "cachepaths");
   analyze_option(confPythonPrefix, "pythonprefix", true);
   analyze_option(confLocalPrefix, "localprefix");
}

static void print_version()
{
   std::cout << "spindle-config-helper version " << PACKAGE_VERSION << std::endl;
}

static bool check_munge_available(bool in_sandbox)
{
   // Check if munge binary exists
   if (access("/usr/bin/munge", X_OK) != 0) {
      return false;
   }

   // If in sandbox, we can't test the daemon
   if (in_sandbox) {
      return true;  // Binary exists, can't test further
   }

   // Try to actually use munge to verify daemon is running
   pid_t pid = fork();
   if (pid == 0) {
      // Child: run munge -n with stdout/stderr redirected to /dev/null
      int devnull = open("/dev/null", O_WRONLY);
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
      execl("/usr/bin/munge", "munge", "-n", (char*)NULL);
      _exit(127);
   } else if (pid > 0) {
      // Parent: wait for child
      int status;
      waitpid(pid, &status, 0);
      return WIFEXITED(status) && WEXITSTATUS(status) == 0;
   }

   return false;  // fork failed
}

static void print_security_analysis(const ConfigMap& confmap, const std::vector<MountPoint>& mounts)
{
   std::cout << "# Security configuration:" << std::endl;

   // Detect if we're in a sandbox
   const char* sandbox_engine = getenv("SANDBOX_ENGINE");
   bool in_sandbox = (sandbox_engine != NULL);

   // Get the current security setting
   auto sec_result = confmap.getValueString(confSecurity);
   std::string current_mode = "unknown";
   if (sec_result.first) {
      current_mode = sec_result.second;
   }

   std::cout << "#   Current mode: " << current_mode << std::endl;

   // Validate based on mode
   if (current_mode == "none") {
      std::cout << "#     WARNING: No authentication enabled (connections are insecure)" << std::endl;
   } else if (current_mode == "keyfile") {
      const char* keydir = get_sec_keydir_path();
      if (keydir) {
         std::cout << "#     Keyfile directory: " << keydir << std::endl;

         // Check if keydir is on a shared (remote) filesystem
         std::string keydir_resolved = resolve_real_path(keydir);
         const MountPoint* mp = find_mount_for_path(keydir_resolved, mounts);

         if (mp) {
            if (mp->is_remote) {
               std::cout << "#     OK: Keyfile directory is on shared filesystem ("
                        << mp->fstype << ")" << std::endl;
            } else {
               std::cout << "#     WARNING: Keyfile directory is on LOCAL filesystem ("
                        << mp->fstype << ")" << std::endl;
               std::cout << "#       Keyfile must be on a filesystem shared across all nodes" << std::endl;
            }
         } else {
            std::cout << "#     WARNING: Cannot determine filesystem type for keyfile directory" << std::endl;
         }
      } else {
         std::cout << "#     Note: Keyfile mode not compiled in" << std::endl;
      }
   } else if (current_mode == "launchmon") {
      std::cout << "#     WARNING: LaunchMON security mode (deprecated/untested)" << std::endl;
   } else if (current_mode == "munge") {
      std::cout << "#     Munge authentication enabled" << std::endl;
   }

   std::cout << "#" << std::endl;
   std::cout << "#   Munge availability:" << std::endl;

   // Always check for munge as a recommendation
   bool munge_binary_exists = (access("/usr/bin/munge", X_OK) == 0);

   if (munge_binary_exists) {
      if (in_sandbox) {
         std::cout << "#     Munge binary found at /usr/bin/munge" << std::endl;
         std::cout << "#     Note: Cannot test munge daemon (running in sandbox)" << std::endl;
      } else {
         bool munge_works = check_munge_available(false);
         if (munge_works) {
            std::cout << "#     Munge is available and working (recommended security option)" << std::endl;
         } else {
            std::cout << "#     Munge binary found but daemon is not running" << std::endl;
            std::cout << "#     Munge provides secure authentication (recommended if available)" << std::endl;
         }
      }
   } else {
      std::cout << "#     Munge not found" << std::endl;
      std::cout << "#     Munge provides secure authentication (recommended if available)" << std::endl;
   }

   std::cout << "#" << std::endl;
}

static void print_usage(const char *progname)
{
   std::cout << "Usage: " << progname << " [OPTIONS]\n\n"
             << "Parse Spindle configuration from command line, environment variables, and\n"
             << "configuration files, then output the resulting configuration in spindle.conf format.\n\n"
             << "Options:\n"
             << "  -h, --help              Display this help message and exit\n"
             << "  -V, --version           Display version information and exit\n"
             << "  --usage                 Display brief usage summary and exit\n\n"
             << "All Spindle configuration options are supported. See spindle-config-helper(1)\n"
             << "for details, or use --usage for a complete list of options.\n\n"
             << "Examples:\n"
             << "  " << progname << " --commpaths=/dev/shm --cachepaths=/tmp\n"
             << "  " << progname << " --config=/etc/spindle.conf\n"
             << "  " << progname << " --launcher=slurm --security=none\n"
             << std::endl;
}

static void format_config_value(const ConfigMap &confmap, SpindleConfigID id,
                                 const SpindleOption &opt)
{
   if (!confmap.isSet(id)) {
      return;
   }

   std::string value;
   bool has_value = false;

   switch (opt.conftype) {
      case cvBool: {
         auto result = confmap.getValueBool(id);
         if (result.first) {
            value = result.second ? "yes" : "no";
            has_value = true;
         }
         break;
      }
      case cvInteger: {
         auto result = confmap.getValueIntegral(id);
         if (result.first) {
            value = std::to_string(result.second);
            has_value = true;
         }
         break;
      }
      case cvString:
      case cvStringOptional:
      case cvList:
      case cvEnum: {
         auto result = confmap.getValueString(id);
         if (result.first) {
            value = result.second;
            has_value = true;
         }
         break;
      }
   }

   if (has_value && opt.cmdline_long) {
      // Convert cmdline option name to config file format
      // e.g., "comm-paths" -> "commpaths"
      std::string key = opt.cmdline_long;
      // Remove dashes using erase-remove idiom
      key.erase(std::remove_if(key.begin(), key.end(),
                               [](char c) { return c == '-'; }),
                key.end());

      std::cout << key << " = " << value << std::endl;
   }
}

int main(int argc, char *argv[])
{
   // Handle -h/--help and -V/--version before config parsing
   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
         print_usage(argv[0]);
         return 0;
      }
      if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
         print_version();
         return 0;
      }
   }

   // Check if application command was provided (after '--')
   bool has_app = false;
   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--") == 0 && i + 1 < argc) {
         has_app = true;
         break;
      }
   }

   // If no application provided, add a dummy one for the parser
   char **modified_argv = argv;
   int modified_argc = argc;
   char *dummy_app = nullptr;

   if (!has_app) {
      dummy_app = strdup("/bin/true");
      modified_argv = new char*[argc + 2];
      for (int i = 0; i < argc; i++) {
         modified_argv[i] = argv[i];
      }
      modified_argv[argc] = strdup("--");
      modified_argv[argc + 1] = dummy_app;
      modified_argc = argc + 2;
   }

   ConfigMap confmap;
   std::string errmsg;

   // Reuse Spindle's existing parsing: cmdline, env vars, config files
   // Pass false for inExecutable since we're not the spindle launcher
   if (!gatherAllConfigInfo(modified_argc, modified_argv, false, confmap, errmsg)) {
      std::cerr << "Error: " << errmsg << std::endl;
      if (!has_app) {
         free(dummy_app);
         free(modified_argv[argc]);
         delete[] modified_argv;
      }
      return 1;
   }

   // Clean up temporary argv if we created one
   if (!has_app) {
      free(dummy_app);
      free(modified_argv[argc]);
      delete[] modified_argv;
   }

   // Parse and analyze mount points
   std::vector<MountPoint> mounts = parse_mounts();

   // Print security analysis
   print_security_analysis(confmap, mounts);

   // Print filesystem recommendations and warnings
   print_filesystem_recommendations(mounts, confmap);

   // Output configuration in spindle.conf format
   const std::map<SpindleConfigID, const SpindleOption&> &options = getOptionsByID();

   for (const auto &pair : options) {
      format_config_value(confmap, pair.first, pair.second);
   }

   return 0;
}
