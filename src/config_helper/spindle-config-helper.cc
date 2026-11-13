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
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <mpi.h>

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
      "overlay",  // Overlay filesystems are considered non-local
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
      // For non-python paths, check if parent is writable
      if (!check_for_python) {
         std::string parent = find_writable_parent(analysis.resolved);
         bool parent_writable = (access(parent.c_str(), W_OK) == 0);

         if (parent_writable) {
            analysis.warnings.push_back("Path does not exist (will be created by Spindle)");
         } else {
            analysis.warnings.push_back("Path does not exist and parent directory is not writable");
            analysis.is_ok = false;
            return analysis;
         }
      } else {
         // Python prefix must exist
         analysis.warnings.push_back("Path does not exist");
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

static std::string get_timestamp()
{
   time_t now = time(nullptr);
   char buf[80];
   strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
   return std::string(buf);
}

static bool test_directory_writable(const std::string& path)
{
   // Try to create a test file in the directory
   std::string testfile = path + "/.spindle_config_test_" + std::to_string(getpid());

   int fd = open(testfile.c_str(), O_CREAT | O_WRONLY | O_EXCL, 0600);
   if (fd < 0) {
      return false;
   }

   close(fd);
   unlink(testfile.c_str());
   return true;
}

static bool validate_tmpdir_path(const std::string& path,
                                  const std::vector<MountPoint>& mounts,
                                  std::string& error_msg)
{
   // Resolve the path (in case it's a symlink)
   std::string resolved = resolve_real_path(path);

   // Check if path exists
   struct stat st;
   if (stat(resolved.c_str(), &st) != 0) {
      error_msg = "Path does not exist: " + path;
      if (resolved != path) {
         error_msg += " (resolves to " + resolved + ")";
      }
      return false;
   }

   // Check if it's a directory
   if (!S_ISDIR(st.st_mode)) {
      error_msg = "Not a directory: " + path;
      return false;
   }

   // Check if writable
   if (!test_directory_writable(resolved)) {
      error_msg = "Directory is not writable: " + path;
      return false;
   }

   // Check if on local filesystem
   const MountPoint* mp = find_mount_for_path(resolved, mounts);
   if (!mp) {
      error_msg = "Cannot determine filesystem type for: " + path;
      return false;
   }

   if (mp->is_remote) {
      error_msg = "Path is on REMOTE filesystem (" + mp->fstype + "): " + path;
      return false;
   }

   return true;
}

static std::string determine_best_path(const std::vector<MountPoint>& mounts,
                                        std::vector<std::string>& warnings)
{
   // Priority order: TMPDIR, TEMPDIR, TMP, then /tmp
   const char* env_vars[] = {"TMPDIR", "TEMPDIR", "TMP", nullptr};
   bool any_env_set = false;
   std::string first_valid_path;

   // Check environment variables
   for (int i = 0; env_vars[i] != nullptr; i++) {
      const char* env_value = getenv(env_vars[i]);

      if (env_value && env_value[0] != '\0') {
         any_env_set = true;
         std::string tmpdir = env_value;
         std::string error_msg;

         if (validate_tmpdir_path(tmpdir, mounts, error_msg)) {
            if (first_valid_path.empty()) {
               first_valid_path = tmpdir;
            }
         }
      }
   }

   // Generate appropriate WARNING or Note for TMPDIR section
   if (!any_env_set) {
      warnings.push_back("WARNING:  The TMPDIR environment variable was not set.");
   } else if (!first_valid_path.empty()) {
      warnings.push_back("Note:  The TMPDIR path is local and writeable.");
   } else {
      warnings.push_back("WARNING:  The TMPDIR environment variable is not a local, writeable filesystem.");
   }

   // Return best path found, or fall back to /tmp
   if (!first_valid_path.empty()) {
      return first_valid_path;
   }

   // Fall back to /tmp
   std::string error_msg;
   if (validate_tmpdir_path("/tmp", mounts, error_msg)) {
      return "/tmp";
   }

   // No suitable path found
   return "";
}

// Helper: Try to run a python binary and get its sys.prefix and sys.path
// Returns true if successful, populating paths
static bool query_python_paths(const std::string& python_cmd, std::set<std::string>& paths)
{
   std::string cmd = python_cmd + " -c \"import sys; print(sys.prefix); print('\\n'.join(sys.path))\" 2>/dev/null";
   FILE* fp = popen(cmd.c_str(), "r");
   if (!fp) {
      return false;
   }

   bool found_any = false;
   char line[PATH_MAX];
   while (fgets(line, sizeof(line), fp) != nullptr) {
      // Remove trailing newline
      size_t len = strlen(line);
      if (len > 0 && line[len-1] == '\n') {
         line[len-1] = '\0';
      }

      if (strlen(line) > 0) {
         // Check if this looks like a valid path
         if (line[0] == '/' && access(line, F_OK) == 0) {
            paths.insert(line);
            found_any = true;
         }
      }
   }

   int status = pclose(fp);
   int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

   return (exit_code == 0 && found_any);
}

// Result structure for python discovery
struct PythonDiscoveryResult {
   std::vector<std::string> paths;
   std::vector<std::string> warnings;
   std::vector<std::string> notes;
};

// Discover Python installations from system python and PYTHONHOME
static PythonDiscoveryResult discover_python_paths()
{
   PythonDiscoveryResult result;
   std::set<std::string> all_paths;

   // Step 1: Save PYTHONHOME if set
   const char* pythonhome_env = getenv("PYTHONHOME");
   std::string saved_pythonhome;
   bool had_pythonhome = (pythonhome_env != nullptr);
   if (had_pythonhome) {
      saved_pythonhome = pythonhome_env;
   }

   // Step 2-3: Unset PYTHONHOME to get clean system python
   if (had_pythonhome) {
      unsetenv("PYTHONHOME");
   }

   std::string system_python;
   bool found_system_python = false;

   // Try python3 first, then python
   FILE* fp = popen("which python3 2>/dev/null", "r");
   if (fp) {
      char path[PATH_MAX];
      if (fgets(path, sizeof(path), fp) != nullptr) {
         size_t len = strlen(path);
         if (len > 0 && path[len-1] == '\n') path[len-1] = '\0';
         if (strlen(path) > 0 && access(path, X_OK) == 0) {
            system_python = path;
            found_system_python = true;
         }
      }
      pclose(fp);
   }

   if (!found_system_python) {
      fp = popen("which python 2>/dev/null", "r");
      if (fp) {
         char path[PATH_MAX];
         if (fgets(path, sizeof(path), fp) != nullptr) {
            size_t len = strlen(path);
            if (len > 0 && path[len-1] == '\n') path[len-1] = '\0';
            if (strlen(path) > 0 && access(path, X_OK) == 0) {
               system_python = path;
               found_system_python = true;
            }
         }
         pclose(fp);
      }
   }

   // Query system python if found
   if (found_system_python) {
      if (query_python_paths(system_python, all_paths)) {
         result.notes.push_back("Found system Python at " + system_python);
      }
   }

   // Step 4: Restore PYTHONHOME
   if (had_pythonhome) {
      setenv("PYTHONHOME", saved_pythonhome.c_str(), 1);
   }

   // Step 5-7: Check PYTHONHOME python if it was set
   if (had_pythonhome) {
      std::string pythonhome_python;
      bool found_pythonhome_python = false;

      // Look for $PYTHONHOME/bin/python3 or $PYTHONHOME/bin/python
      std::string py3_path = saved_pythonhome + "/bin/python3";
      std::string py_path = saved_pythonhome + "/bin/python";

      if (access(py3_path.c_str(), X_OK) == 0) {
         pythonhome_python = py3_path;
         found_pythonhome_python = true;
      } else if (access(py_path.c_str(), X_OK) == 0) {
         pythonhome_python = py_path;
         found_pythonhome_python = true;
      }

      if (found_pythonhome_python) {
         // Try to run it and get paths
         std::set<std::string> pythonhome_paths;
         if (query_python_paths(pythonhome_python, pythonhome_paths)) {
            // Success! Add PYTHONHOME paths first (they take priority)
            for (const auto& path : pythonhome_paths) {
               all_paths.insert(path);
            }
            result.notes.push_back("Found PYTHONHOME Python at " + pythonhome_python);
         } else {
            result.warnings.push_back("PYTHONHOME is set to '" + saved_pythonhome +
                                     "' but running " + pythonhome_python + " failed");
         }
      } else {
         result.warnings.push_back("PYTHONHOME is set to '" + saved_pythonhome +
                                  "' but no python binary found there");
      }
   } else {
      result.notes.push_back("PYTHONHOME not set, using system Python");
   }

   // Step 11: Check if we found any python at all
   if (!found_system_python && !had_pythonhome) {
      result.warnings.push_back("No Python installation found");
   } else if (!found_system_python && had_pythonhome) {
      // Already warned about PYTHONHOME issues above
   }

   result.paths = std::vector<std::string>(all_paths.begin(), all_paths.end());
   return result;
}

// Deduplicate paths by keeping only top-level directories
// If /foo is in the list and /foo/bar is also in the list, keep only /foo
static std::vector<std::string> deduplicate_paths(const std::vector<std::string>& paths)
{
   if (paths.empty()) return paths;

   // Sort by path length (shortest first)
   std::vector<std::string> sorted_paths = paths;
   std::sort(sorted_paths.begin(), sorted_paths.end(),
             [](const std::string& a, const std::string& b) {
                return a.length() < b.length();
             });

   std::vector<std::string> result;
   for (const auto& path : sorted_paths) {
      bool is_subpath = false;

      // Check if this path is under any already-kept path
      for (const auto& kept : result) {
         // Check if path starts with kept/
         if (path.size() > kept.size() &&
             path.compare(0, kept.size(), kept) == 0 &&
             (path[kept.size()] == '/' || kept == "/")) {
            is_subpath = true;
            break;
         }
      }

      if (!is_subpath) {
         result.push_back(path);
      }
   }

   return result;
}

// Check if a path contains Python libraries
static bool is_valid_python_prefix(const std::string& path)
{
   // Check for lib/python* or lib64/python* directories
   std::string lib_path = path + "/lib";
   std::string lib64_path = path + "/lib64";

   DIR* dir = opendir(lib_path.c_str());
   if (dir) {
      struct dirent* entry;
      while ((entry = readdir(dir)) != nullptr) {
         if (strncmp(entry->d_name, "python", 6) == 0) {
            closedir(dir);
            return true;
         }
      }
      closedir(dir);
   }

   dir = opendir(lib64_path.c_str());
   if (dir) {
      struct dirent* entry;
      while ((entry = readdir(dir)) != nullptr) {
         if (strncmp(entry->d_name, "python", 6) == 0) {
            closedir(dir);
            return true;
         }
      }
      closedir(dir);
   }

   return false;
}

// Wrapper that returns just paths (for backward compatibility)
static std::vector<std::string> discover_python_prefixes()
{
   PythonDiscoveryResult result = discover_python_paths();
   return deduplicate_paths(result.paths);
}

static bool has_remote_mount_under(const std::string& prefix, const std::vector<MountPoint>& mounts)
{
   // Check if any remote mount exists under this prefix
   std::string prefix_slash = prefix;
   if (prefix != "/" && prefix_slash.back() != '/') {
      prefix_slash += "/";
   }

   for (const auto& mp : mounts) {
      if (!mp.is_remote) continue;

      // Check if this remote mount is under prefix
      if (mp.path == prefix || mp.path.find(prefix_slash) == 0) {
         return true;
      }
   }
   return false;
}

static std::vector<std::string> generate_localprefix(const std::vector<MountPoint>& mounts)
{
   std::vector<std::string> local_dirs;

   // Always include kernel filesystems
   local_dirs.push_back("/proc");
   local_dirs.push_back("/sys");
   local_dirs.push_back("/dev");

   // Get top-level directories
   DIR* dir = opendir("/");
   if (!dir) {
      return local_dirs;
   }

   struct dirent* entry;
   while ((entry = readdir(dir)) != nullptr) {
      if (entry->d_name[0] == '.') continue;

      std::string top_level = std::string("/") + entry->d_name;

      // Skip if already added (kernel filesystems)
      if (top_level == "/proc" || top_level == "/sys" || top_level == "/dev") {
         continue;
      }

      // Check if directory
      struct stat st;
      if (stat(top_level.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
         continue;
      }

      // Skip if has remote mounts underneath
      if (has_remote_mount_under(top_level, mounts)) {
         continue;
      }

      local_dirs.push_back(top_level);
   }
   closedir(dir);

   // Sort for consistent output (kernel filesystems first)
   std::sort(local_dirs.begin() + 3, local_dirs.end());

   return local_dirs;
}

static std::string get_hostname_for_ssh()
{
   char hostname[256];
   if (gethostname(hostname, sizeof(hostname)) != 0) {
      return "";
   }
   return std::string(hostname);
}

// Forward declaration
static bool test_ssh_connectivity(const std::string& hostname, std::string& error_msg);

// Structure to hold SSH test results for MPI communication
struct SSHTestResult {
   int rank;
   int target_rank;
   bool success;
   char error_msg[256];
   char source_host[256];
   char target_host[256];
};

// Ring topology SSH test: each rank tests SSH to next rank
static void test_ssh_ring(int mpi_rank, int mpi_size, std::vector<std::string>& warnings)
{
   // Get local hostname
   std::string local_hostname = get_hostname_for_ssh();

   // Exchange hostnames among all ranks using MPI_Allgather
   const int MAX_HOSTNAME_LEN = 256;
   char local_host[MAX_HOSTNAME_LEN];
   char all_hosts[MAX_HOSTNAME_LEN * mpi_size];

   strncpy(local_host, local_hostname.c_str(), MAX_HOSTNAME_LEN - 1);
   local_host[MAX_HOSTNAME_LEN - 1] = '\0';

   MPI_Allgather(local_host, MAX_HOSTNAME_LEN, MPI_CHAR,
                 all_hosts, MAX_HOSTNAME_LEN, MPI_CHAR,
                 MPI_COMM_WORLD);

   // Determine target: (rank + 1) % size
   int target_rank = (mpi_rank + 1) % mpi_size;
   std::string target_hostname(&all_hosts[target_rank * MAX_HOSTNAME_LEN]);

   // Test SSH to target
   SSHTestResult result;
   result.rank = mpi_rank;
   result.target_rank = target_rank;
   strncpy(result.source_host, local_hostname.c_str(), 255);
   result.source_host[255] = '\0';
   strncpy(result.target_host, target_hostname.c_str(), 255);
   result.target_host[255] = '\0';

   std::string ssh_error;
   result.success = test_ssh_connectivity(target_hostname, ssh_error);
   strncpy(result.error_msg, ssh_error.c_str(), 255);
   result.error_msg[255] = '\0';

   // Gather all results to rank 0
   SSHTestResult all_results[mpi_size];
   MPI_Gather(&result, sizeof(SSHTestResult), MPI_BYTE,
              all_results, sizeof(SSHTestResult), MPI_BYTE,
              0, MPI_COMM_WORLD);

   // Rank 0 processes and reports results
   if (mpi_rank == 0) {
      int successful_tests = 0;
      bool all_passed = true;

      for (int i = 0; i < mpi_size; i++) {
         if (all_results[i].success) {
            successful_tests++;
         } else {
            all_passed = false;
         }
      }

      // Count unique nodes from the hostname list
      std::set<std::string> unique_nodes;
      for (int i = 0; i < mpi_size; i++) {
         std::string hostname(&all_hosts[i * MAX_HOSTNAME_LEN]);
         if (!hostname.empty()) {
            unique_nodes.insert(hostname);
         }
      }
      int num_nodes = unique_nodes.size();

      if (all_passed) {
         char msg[512];
         snprintf(msg, sizeof(msg), "Note:  Tested ssh successfully under MPI using %d ranks across %d nodes.",
                  mpi_size, num_nodes);
         warnings.push_back(std::string(msg));
      } else {
         warnings.push_back("WARNING:  Executing under MPI, but ssh communication among compute nodes failed.");

         // Report detailed failures as additional lines
         char msg[512];
         for (int i = 0; i < mpi_size; i++) {
            if (!all_results[i].success) {
               snprintf(msg, sizeof(msg), "#   Rank %d (%s) -> Rank %d (%s): FAILED - %s",
                       all_results[i].rank, all_results[i].source_host,
                       all_results[i].target_rank, all_results[i].target_host,
                       all_results[i].error_msg);
               warnings.push_back(std::string(msg));
            }
         }
      }
   }
}

static bool test_ssh_connectivity(const std::string& hostname, std::string& error_msg)
{
   if (hostname.empty()) {
      error_msg = "Could not determine hostname";
      return false;
   }

   // Build SSH command: ssh -o BatchMode=yes -o ConnectTimeout=10 <hostname> /bin/true
   std::string cmd = "ssh -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=no " +
                     hostname + " /bin/true 2>&1";

   FILE* fp = popen(cmd.c_str(), "r");
   if (!fp) {
      error_msg = "Failed to execute ssh command";
      return false;
   }

   // Read any error output
   char buffer[256];
   std::string output;
   while (fgets(buffer, sizeof(buffer), fp) != nullptr) {
      output += buffer;
   }

   int status = pclose(fp);
   int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

   if (exit_code == 0) {
      return true;
   }

   // SSH failed - extract useful error info
   if (output.find("Permission denied") != std::string::npos) {
      error_msg = "SSH authentication failed (check SSH keys)";
   } else if (output.find("Connection refused") != std::string::npos) {
      error_msg = "SSH connection refused";
   } else if (output.find("Connection timed out") != std::string::npos) {
      error_msg = "SSH connection timed out";
   } else if (output.find("No route to host") != std::string::npos) {
      error_msg = "No route to host";
   } else if (!output.empty()) {
      // Truncate long error messages
      error_msg = output.substr(0, 100);
      if (output.length() > 100) error_msg += "...";
   } else {
      error_msg = "SSH test failed (exit code " + std::to_string(exit_code) + ")";
   }

   return false;
}

// Forward declarations
static void print_config_header(std::vector<std::string>& warnings);

// Parse a spindle.conf file and return key-value pairs
// Returns: map of key -> value pairs
// Ignores comments and blank lines
static void print_config_header(std::vector<std::string>& warnings)
{
   std::cout << "# spindle.conf generated by spindle-config-helper version "
             << PACKAGE_VERSION << std::endl;
   std::cout << "# Generated: " << get_timestamp() << std::endl;
   std::cout << "#" << std::endl;

   // Print each warning/note on its own line (already prefixed with # WARNING: or # Note:)
   for (const auto& warning : warnings) {
      std::cout << "# " << warning << std::endl;
   }

   if (!warnings.empty()) {
      std::cout << "#" << std::endl;
   }
}

static void print_path_variable(const std::string& name, const std::string& value,
                                 const std::vector<std::string>& comments)
{
   // Print comments above the variable
   for (const auto& comment : comments) {
      std::cout << "# " << comment << std::endl;
   }

   // If value is empty, comment out the line
   if (value.empty()) {
      std::cout << "# " << name << " =" << std::endl;
   } else {
      std::cout << name << " = " << value << std::endl;
   }
   // No blank line after - we put blank lines BEFORE sections instead
}

static void generate_default_config(const ConfigMap& confmap,
                                     const std::vector<MountPoint>& mounts,
                                     std::vector<std::string>& header_warnings,
                                     int mpi_rank, int mpi_size)
{
   // Determine best path for commpaths/cachepaths if using defaults
   std::string recommended_path;
   bool need_path_recommendation = false;

   // Check if commpaths or cachepaths are using the default $TMPDIR
   auto commpaths_result = confmap.getValueString(confCommPaths);
   auto cachepaths_result = confmap.getValueString(confCachePaths);

   bool commpaths_is_default = (commpaths_result.first && commpaths_result.second == "$TMPDIR");
   bool cachepaths_is_default = (cachepaths_result.first && cachepaths_result.second == "$TMPDIR");

   if (commpaths_is_default || cachepaths_is_default) {
      need_path_recommendation = true;
      std::vector<std::string> path_warnings;
      recommended_path = determine_best_path(mounts, path_warnings);

      // Add path-related warnings to header
      for (const auto& warning : path_warnings) {
         header_warnings.push_back(warning);
      }

      if (recommended_path.empty()) {
         header_warnings.push_back("CRITICAL: No suitable local filesystem found for commpaths/cachepaths");
         header_warnings.push_back("          Cannot generate valid configuration");
         // Print header with errors and exit
         print_config_header(header_warnings);
         return;
      }
   }

   // SSH test results already collected in header_warnings (done in main before calling this function)

   // Header
   print_config_header(header_warnings);

   // Generate the 4 path-based variables with analysis
   auto output_path_option = [&](SpindleConfigID config_id, const char* name,
                                  bool check_python = false) {
      // Check if this option is set (either default or user-specified)
      if (!confmap.isSet(config_id)) {
         return;
      }

      auto result = confmap.getValueString(config_id);
      if (!result.first || result.second.empty()) {
         return;
      }

      std::string paths = result.second;
      std::vector<std::string> comments;

      // If this is commpaths or cachepaths with $TMPDIR default, use recommended path
      if (need_path_recommendation &&
          (config_id == confCommPaths || config_id == confCachePaths) &&
          paths == "$TMPDIR" && !recommended_path.empty()) {
         paths = recommended_path;
      }

      // For python-prefix (Spindle parameter), discover additional Python installations
      std::vector<std::string> discovered_pythons;
      PythonDiscoveryResult python_discovery;
      if (config_id == confPythonPrefix) {
         python_discovery = discover_python_paths();
         discovered_pythons = deduplicate_paths(python_discovery.paths);
      }

      // Analyze each path in the colon-separated list
      std::vector<std::string> path_list;
      std::vector<PathAnalysis> analyses;
      size_t pos = 0;
      while (pos < paths.length()) {
         size_t end = paths.find(':', pos);
         if (end == std::string::npos) {
            end = paths.length();
         }
         std::string path = paths.substr(pos, end - pos);
         if (!path.empty()) {
            path_list.push_back(path);
            analyses.push_back(analyze_path(path, mounts, check_python));
         }
         pos = end + 1;
      }

      // Add discovered Python paths to the list
      if (config_id == confPythonPrefix) {
         // Add Python discovery warnings and notes first
         for (const auto& warning : python_discovery.warnings) {
            comments.push_back("WARNING: " + warning);
         }
         for (const auto& note : python_discovery.notes) {
            comments.push_back("NOTE: " + note);
         }

         for (const auto& pypath : discovered_pythons) {
            // Check if not already in list
            bool already_present = false;
            for (const auto& existing : path_list) {
               PathAnalysis ex = analyze_path(existing, mounts, check_python);
               if (ex.resolved == resolve_real_path(pypath)) {
                  already_present = true;
                  break;
               }
            }
            if (!already_present) {
               path_list.push_back(pypath);
               analyses.push_back(analyze_path(pypath, mounts, check_python));
               comments.push_back("NOTE: Added " + pypath + " from Python discovery");
            }
         }
      }

      // Separate usable from unusable paths
      std::vector<std::string> good_paths, bad_paths;

      for (size_t i = 0; i < path_list.size(); i++) {
         bool has_critical_issues = false;

         // For python-prefix (Spindle parameter), check for Python installation issues
         if (check_python) {
            for (const auto& warning : analyses[i].warnings) {
               if (warning.find("No bin/python") != std::string::npos ||
                   warning.find("Path does not exist") != std::string::npos ||
                   warning.find("parent directory is not writable") != std::string::npos) {
                  has_critical_issues = true;
                  break;
               }
            }
         } else {
            // For commpaths/cachepaths/localprefix
            for (const auto& warning : analyses[i].warnings) {
               if (warning.find("parent directory is not writable") != std::string::npos ||
                   warning.find("REMOTE filesystem") != std::string::npos) {
                  has_critical_issues = true;
                  break;
               }
            }
         }

         if (has_critical_issues) {
            bad_paths.push_back(path_list[i]);
         } else {
            good_paths.push_back(path_list[i]);
         }
      }

      // If no good paths, add fallback
      if (good_paths.empty() && (config_id == confCommPaths || config_id == confCachePaths)) {
         if (!recommended_path.empty()) {
            good_paths.push_back(recommended_path);
            comments.push_back("NOTE: No usable paths found, using " + recommended_path);
         }
      }

      // Silently drop bad paths - no need to warn about them

      // Use only good paths in output
      if (!good_paths.empty()) {
         std::string filtered;
         for (const auto& p : good_paths) {
            if (!filtered.empty()) filtered += ":";
            filtered += p;
         }
         paths = filtered;
      } else {
         // No good paths at all - output commented line
         paths = "";
         if (check_python) {
            comments.push_back("WARNING: No valid Python installations found");
         }
      }

      // Add comment for usable paths if no issues
      bool has_issues = !bad_paths.empty();
      if (!has_issues && !good_paths.empty()) {
         PathAnalysis first = analyze_path(good_paths[0], mounts, check_python);
         if (first.mount && !first.mount->is_remote) {
            comments.push_back(std::string(name) + ": Local " + first.mount->fstype +
                             " filesystem, " + std::to_string(first.mount->avail_gb) + "GB available");
         } else if (is_kernel_filesystem(first.resolved)) {
            comments.push_back(std::string(name) + ": Kernel filesystem");
         }
      }

      print_path_variable(name, paths, comments);
   };

   // Add comment block for commpaths and cachepaths
   std::cout << "#" << std::endl;
   std::cout << "# commpaths and cachepaths" << std::endl;
   std::cout << "# These two settings are colon-separated lists of local (on-node) storage that can be used for Spindle" << std::endl;
   std::cout << "#   sockets and caches, respectively.  If the entries below are commented out, they either pointed to" << std::endl;
   std::cout << "#   a non-local filesystem or were not writeable by this user.  Both of these are based on the" << std::endl;
   std::cout << "#   TMPDIR environment variable; fallback is /tmp." << std::endl;
   output_path_option(confCommPaths, "commpaths");
   output_path_option(confCachePaths, "cachepaths");

   // Add comment block for python-prefix
   std::cout << "#" << std::endl;
   std::cout << "# python-prefix" << std::endl;
   std::cout << "# This is a colon-separated list of paths that contain python packages (including site-specific" << std::endl;
   std::cout << "#   packages).  The directories will include PYTHONHOME (if specified) and the results of" << std::endl;
   std::cout << "#   checking python's sys.prefix and sys.path.  If the entry is commented out, the program could" << std::endl;
   std::cout << "#   not locate python libraries." << std::endl;

   output_path_option(confPythonPrefix, "python-prefix", true);

   // Special handling for localprefix - generate from top-level dirs without remote mounts
   auto localprefix_result = confmap.getValueString(confLocalPrefix);
   if (localprefix_result.first) {
      std::vector<std::string> generated_locals = generate_localprefix(mounts);
      std::vector<std::string> comments;

      // Check if user specified additional paths (beyond defaults)
      std::string user_value = localprefix_result.second;
      std::set<std::string> user_paths;

      // Parse user's colon-separated list
      size_t pos = 0;
      while (pos < user_value.length()) {
         size_t end = user_value.find(':', pos);
         if (end == std::string::npos) end = user_value.length();

         std::string path = user_value.substr(pos, end - pos);
         // Strip trailing slash for comparison
         if (!path.empty() && path.back() == '/') path.pop_back();
         if (!path.empty()) user_paths.insert(path);

         pos = end + 1;
      }

      // Add generated paths to user paths
      std::set<std::string> all_paths(user_paths);
      for (const auto& gpath : generated_locals) {
         all_paths.insert(gpath);
      }

      // Validate user additions
      for (const auto& upath : user_paths) {
         // Check if this was NOT auto-generated (meaning user added it)
         bool was_generated = (std::find(generated_locals.begin(), generated_locals.end(), upath) != generated_locals.end());
         if (!was_generated) {
            // User added this - validate it
            if (has_remote_mount_under(upath, mounts)) {
               comments.push_back("WARNING: User-specified path has remote mounts underneath: " + upath);
            } else {
               comments.push_back("NOTE: User added: " + upath);
            }
         }
      }

      // Build output with trailing slashes
      std::string output_value;
      for (const auto& dir : all_paths) {
         if (!output_value.empty()) output_value += ":";
         output_value += (dir == "/" ? "/" : dir + "/");
      }

      // Add comment block for local-prefix
      std::cout << "#" << std::endl;
      std::cout << "# local-prefix" << std::endl;
      std::cout << "# This is a colon-separated list of paths that Spindle should treat as node-local filesystems" << std::endl;
      std::cout << "# (i.e., Spindle will not attempt to cache anything underneath these directories).  Overlay" << std::endl;
      std::cout << "# filesystems are assumed to be non-local." << std::endl;

      print_path_variable("local-prefix", output_value, comments);
   }
}

static void print_version()
{
   std::cout << "spindle-config-helper version " << PACKAGE_VERSION << std::endl;
}

static void print_config_help()
{
   std::cout << "Spindle Configuration File Syntax\n";
   std::cout << "==================================\n\n";
   std::cout << "Config files use the format:  parameter_name = value\n";
   std::cout << "Lines starting with '#' are comments.\n";
   std::cout << "Single-letter command-line options (like -r) are NOT supported in config files.\n";
   std::cout << "Use the long form with hyphens as shown below.\n\n";
   std::cout << "Available configuration parameters:\n";
   std::cout << "-----------------------------------\n\n";

   // Initialize options list first
   initOptionsList();

   // Get options from Spindle's config system
   const std::map<std::string, const SpindleOption&> &opts = getOptionsByLongName();

   for (const auto& pair : opts) {
      const SpindleOption& opt = pair.second;

      // Skip command-line-only options (like confCmdlineOnly, confCmdlineNewgroup)
      if (opt.cmdline_long == nullptr || opt.cmdline_long[0] == '\0') {
         continue;
      }

      std::cout << opt.cmdline_long << " = ";

      // Show value format based on type
      switch (opt.conftype) {
         case cvBool:
            std::cout << "true/false";
            break;
         case cvInteger:
            std::cout << "<number>";
            break;
         case cvString:
         case cvStringOptional:
            std::cout << "<string>";
            break;
         case cvList:
            std::cout << "<path1>:<path2>:...";
            break;
         case cvEnum:
            if (!opt.enum_values.empty()) {
               bool first = true;
               for (const char* val : opt.enum_values) {
                  if (!first) std::cout << "/";
                  std::cout << val;
                  first = false;
               }
            }
            break;
      }

      if (opt.default_value && opt.default_value[0] != '\0') {
         std::cout << "  (default: " << opt.default_value << ")";
      }

      std::cout << "\n";

      if (opt.helptext && opt.helptext[0] != '\0') {
         std::cout << "    " << opt.helptext << "\n";
      }

      std::cout << "\n";
   }

   std::cout << "For more information, see: spindle --help\n";
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

static void print_usage(const char *progname)
{
   std::cout << "Usage: " << progname << " [OPTIONS]\n\n"
             << "Generate and validate Spindle configuration files for HPC environments.\n\n"
             << "INSTRUCTIONS:\n"
             << "  1) Set PYTHONHOME to the python your programs will use.\n"
             << "  2) Set TMPDIR to node-local temporary storage (e.g., /tmp).\n"
             << "  3) Run " << progname << " as you would any other parallel\n"
             << "     program, e.g., flux run -N2 -n2 " << progname << ".\n"
             << "  4) Check the output, then redirect/copypaste to\n"
             << "     ~/.spindle/spindle.conf\n\n"
             << "See the output for explanations of how spindle-config-helper chooses\n"
             << "  paths and validates the environment.\n\n"
             << "OPTIONS:\n"
             << "  -h, --help              Display this help message and exit\n"
             << "  -V, --version           Display version information and exit\n"
             << "  --config-help           Display config file syntax for all Spindle options\n\n"
             << "SEE ALSO:\n"
             << "  spindle(1), spindle-config-helper(1)\n\n"
             << "VERSION:\n"
             << "  spindle-config-helper " << PACKAGE_VERSION << "\n"
             << std::endl;
}

// Detect if we're running under an MPI launcher by checking for MPI-related environment variables
static bool running_under_mpi()
{
   // Check for common MPI launcher environment variables
   const char* mpi_env_vars[] = {
      "PMI_RANK",           // PMI-based launchers
      "PMI_SIZE",
      "PMIX_RANK",          // PMIx
      "SLURM_PROCID",       // Slurm
      "SLURM_NTASKS",
      "FLUX_TASK_RANK",     // Flux
      "FLUX_JOB_SIZE",
      "OMPI_COMM_WORLD_RANK",  // OpenMPI
      "MPI_LOCALRANKID",    // Intel MPI
      nullptr
   };

   for (int i = 0; mpi_env_vars[i] != nullptr; i++) {
      if (getenv(mpi_env_vars[i]) != nullptr) {
         return true;
      }
   }

   return false;
}

int main(int argc, char *argv[])
{
   // Initialize MPI if running under an MPI launcher
   int mpi_rank = 0, mpi_size = 1;
   bool is_mpi_job = running_under_mpi();
   int mpi_initialized = 0;

   if (is_mpi_job) {
      MPI_Initialized(&mpi_initialized);

      if (!mpi_initialized) {
         MPI_Init(&argc, &argv);
      }
      MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
      MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);
   }

   // Handle -h/--help, -V/--version, and --config-help
   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
         if (mpi_rank == 0) {
            print_usage(argv[0]);
         }
         if (is_mpi_job) {
            MPI_Finalize();
         }
         return 0;
      }
      if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
         if (mpi_rank == 0) {
            print_version();
         }
         if (is_mpi_job) {
            MPI_Finalize();
         }
         return 0;
      }
      if (strcmp(argv[i], "--config-help") == 0) {
         if (mpi_rank == 0) {
            print_config_help();
         }
         if (is_mpi_job) {
            MPI_Finalize();
         }
         return 0;
      }
      // Any other argument is an error (only -h/-V/--config-help allowed)
      if (argv[i][0] == '-') {
         if (mpi_rank == 0) {
            std::cerr << "Error: Unknown option '" << argv[i] << "'" << std::endl;
            std::cerr << "Try '" << argv[0] << " --help' for more information." << std::endl;
         }
         if (is_mpi_job) {
            MPI_Finalize();
         }
         return 1;
      }
   }

   // Build argv for gatherAllConfigInfo (needs dummy app)
   std::vector<char*> new_argv;
   new_argv.push_back(argv[0]);  // Program name

   // Add dummy app (required by Spindle's parser)
   char *dummy_sep = strdup("--");
   char *dummy_app = strdup("/bin/true");
   new_argv.push_back(dummy_sep);
   new_argv.push_back(dummy_app);

   char **modified_argv = new_argv.data();
   int modified_argc = new_argv.size();

   ConfigMap confmap;
   std::string errmsg;

   // Call Spindle's config parser to get defaults
   // Pass false for inExecutable since we're not the spindle launcher
   if (!gatherAllConfigInfo(modified_argc, modified_argv, false, confmap, errmsg)) {
      std::cerr << "Error: " << errmsg << std::endl;
      free(dummy_app);
      free(dummy_sep);
      return 1;
   }

   // Clean up temporary argv
   free(dummy_app);
   free(dummy_sep);

   // Parse and analyze mount points (all ranks need this for SSH test)
   std::vector<MountPoint> mounts = parse_mounts();

   // Collect header warnings
   std::vector<std::string> header_warnings;

   // Generate PYTHONHOME warning/note (only rank 0 needs to do this since it's the only one printing)
   // The detailed warnings/notes will be added per-path in the python-prefix section
   if (mpi_rank == 0) {
      const char* pythonhome_env = getenv("PYTHONHOME");
      if (!pythonhome_env || pythonhome_env[0] == '\0') {
         header_warnings.push_back("WARNING:  The PYTHONHOME environment variable was not set.");
      } else {
         // More detailed warnings/notes will appear in the python-prefix section
         // Just note that it was set here
         header_warnings.push_back("Note:  The PYTHONHOME environment variable is set.");
      }
   }

   // Generate Munge warning/note (only rank 0)
   if (mpi_rank == 0) {
      const char* sandbox_engine = getenv("SANDBOX_ENGINE");
      bool in_sandbox = (sandbox_engine != nullptr);

      if (check_munge_available(in_sandbox)) {
         header_warnings.push_back("Note:  Munge is working as expected.");
      } else {
         header_warnings.push_back("WARNING:  Munge is either not set up or not supported.");
      }
   }

   // ALL RANKS: Test SSH connectivity using ring topology (uses collective MPI operations)
   // MPI/SSH warning will be generated by test_ssh_ring or afterward
   if (!is_mpi_job) {
      // Not running under MPI
      if (mpi_rank == 0) {
         header_warnings.push_back("WARNING:  Not executing under MPI.  This environment may not match what is used by parallel programs.");
      }
   } else if (mpi_size > 1) {
      test_ssh_ring(mpi_rank, mpi_size, header_warnings);
   } else if (mpi_rank == 0) {
      // Single node - add single node warning
      header_warnings.push_back("WARNING:  Executing under MPI with a single compute node.  This may not give accurate ssh results.");
   }

   // Only rank 0 generates and prints output
   if (mpi_rank == 0) {
      // Generate config to stdout
      generate_default_config(confmap, mounts, header_warnings, mpi_rank, mpi_size);
   }

   if (is_mpi_job) {
      MPI_Finalize();
   }
   return 0;
}
