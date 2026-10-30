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

#include "ldcs_cache.h"
#include "spindle_debug.h"
#include "ccwarns.h"
#include "ldcs_api.h"
#include "global_name.h"

#include <string>
#include <unordered_map>
#include <set>
#include <utility>
#include <list>
#include <cassert>

#include <dirent.h>

using namespace std;

struct file_location_t {
   string dirname;
   string filename;
   
   file_location_t(string d) : dirname(d) {}
   file_location_t(string d, string f) : dirname(d), filename(f) {}
   bool operator<(const file_location_t &other) const {
      if (dirname != other.dirname) {
         return dirname < other.dirname;
      }
      return filename < other.filename;
   }
   bool operator==(const file_location_t &other) const {
      return dirname == other.dirname && filename == other.filename;
   }
};

struct file_location_hash
{
   size_t operator()(const file_location_t &a) const {
      return hash<string>()(a.dirname) ^ (hash<string>()(a.filename) << 1);
   }
};
   
struct cached_contents_t {
   cached_contents_t() : buffer(NULL), buffer_size(0) {}
   void reset() { path = string(); buffer = NULL; buffer_size = 0; }
   string path;
   void *buffer;
   size_t buffer_size;
};

struct entry_t {
   file_location_t path;
   cached_contents_t file_contents;
   cached_contents_t dso_contents;
   string alias;
   int errcode;
   bool replication;
   list<entry_t*> direntries;
   entry_t(const file_location_t &path_) : path(path_), errcode(0), replication(false) {}
};

typedef unordered_map<file_location_t, entry_t*, file_location_hash> cache_t;
static cache_t cache;

int ldcs_cache_init() {
   init_global_name_list();   
   return 0;   
}

static void addDirectory(string dirname, bool exists)
{
   file_location_t key(dirname);
   cache_t::iterator i = cache.find(key);
   if (i != cache.end())
      return;
   entry_t *newentry = new entry_t(key);
   if (!exists)
      newentry->errcode = -1;
   cache.insert(make_pair(key, newentry)); 
}

static void addFile(string dirname, string filename)
{
   file_location_t key(dirname, filename);
   cache_t::iterator i = cache.find(key);
   if (i != cache.end())
      return;
   entry_t *newentry = new entry_t(key);
   cache.insert(make_pair(key, newentry));

   file_location_t dirkey(dirname);
   cache_t::iterator j = cache.find(dirkey);
   assert(j != cache.end());
   entry_t *dir = j->second;
   dir->direntries.push_back(newentry);
}
   
ldcs_cache_result_t ldcs_cache_findDirInCache(char *dirname)
{
   file_location_t key(dirname);
   
   cache_t::iterator i = cache.find(key);
   if (i == cache.end()) {
      debug_printf3("Looked up dir %s: Not parsed\n", dirname);
      return LDCS_CACHE_DIR_NOT_PARSED;
   }
   entry_t *e = i->second;
   if (e->errcode == -1) {
      debug_printf3("Looked up dir %s: Does not exist\n", dirname);
      return LDCS_CACHE_DIR_PARSED_AND_NOT_EXISTS;
   }
   else {
      debug_printf3("Looked up dir %s: Exists\n", dirname);
      return LDCS_CACHE_DIR_PARSED_AND_EXISTS;
   }
}

ldcs_cache_result_t ldcs_cache_findFileDirInCache(char *filename, char *dirname,
                                                  ldcs_hash_fileobj_t objt,
                                                  char **localpath, int *errcode)
{
   file_location_t key(dirname, filename);
   cache_t::iterator i = cache.find(key);
   if (i == cache.end()) {
      debug_printf3("Looked up %s %s/%s: Not found\n",
                    objt == LDCS_CACHE_FILEOBJ_DSO ? "dso" : (objt == LDCS_CACHE_FILEOBJ_FILE ? "file" : "any"),
                    dirname, filename);
      *localpath = NULL;
      *errcode = 0;
      return LDCS_CACHE_FILE_NOT_FOUND;
   }
   entry_t *e = i->second;
   cached_contents_t *contents = NULL;
   switch (objt) {
      case LDCS_CACHE_FILEOBJ_DSO:
         contents = &(e->dso_contents);
         break;
      case LDCS_CACHE_FILEOBJ_FILE:
         contents = &(e->file_contents);
         break;
      case LDCS_CACHE_FILEOBJ_EITHER:
         if (!e->dso_contents.path.empty())
            contents = &(e->dso_contents);
         else
            contents = &(e->file_contents);
         break;
   }
   *localpath = !contents->path.empty() ? const_cast<char *>(contents->path.c_str()) : NULL;
   *errcode = e->errcode;
   debug_printf3("Looked up %s %s/%s: errcode - %d, local - %s\n",
                 objt == LDCS_CACHE_FILEOBJ_DSO ? "dso" : (objt == LDCS_CACHE_FILEOBJ_FILE ? "file" : "any"),
                 dirname, filename, 
                 *errcode, *localpath);
   return LDCS_CACHE_FILE_FOUND;
}

ldcs_cache_result_t ldcs_cache_getAlias(char *filename, char *dirname, char **alias)
{
   file_location_t key(dirname, filename);
   cache_t::iterator i = cache.find(key);
   if (i == cache.end()) {
      *alias = NULL;
      return LDCS_CACHE_FILE_NOT_FOUND;
   };
   entry_t *e = i->second;
   if (e->alias.empty()) {
      *alias = NULL;
      return LDCS_CACHE_FILE_FOUND;
   }
   *alias = const_cast<char *>(e->alias.c_str());
   return LDCS_CACHE_FILE_FOUND;
}

ldcs_cache_result_t ldcs_cache_isReplicated(char *filename, char *dirname, int *replication)
{
   file_location_t key(dirname, filename);
   cache_t::iterator i = cache.find(key);
   if (i == cache.end()) {
      *replication = 0;
      return LDCS_CACHE_FILE_NOT_FOUND;
   };
   *replication = i->second->replication ? 1 : 0;
   return LDCS_CACHE_FILE_FOUND;
}

static void lsAndCacheFiles(string dirname, size_t *bytesread);

ldcs_cache_result_t ldcs_cache_processDirectory(char *dirname, size_t *bytesread) {
   string dname(dirname);
   if (bytesread) *bytesread = 0;
   debug_printf3("Processing directory %s\n", dirname);
   file_location_t key(dname);   
   cache_t::iterator i = cache.find(key);
   if (i != cache.end()) {
      debug_printf3("Directory %s already parsed\n", dirname);
      return LDCS_CACHE_DIR_PARSED_AND_EXISTS;
   }

   lsAndCacheFiles(dname, bytesread);
   return ldcs_cache_findDirInCache(dirname);
}

ldcs_cache_result_t ldcs_cache_updateAlias(char *filename, char *dirname, char *alias_to)
{
   file_location_t key(dirname, filename);
   cache_t::iterator i = cache.find(key);
   if (i == cache.end()) {
      err_printf("Asked to update alias %s/%s, but not in cache\n", dirname, filename);
      return LDCS_CACHE_FILE_NOT_FOUND;
   };
   debug_printf3("Updating cache of %s/%s with alias information\n", dirname, filename);   
   i->second->alias = string(alias_to);
   return LDCS_CACHE_FILE_FOUND;
}

ldcs_cache_result_t ldcs_cache_updateReplication(char *filename, char *dirname, int replication)
{
   file_location_t key(dirname, filename);
   cache_t::iterator i = cache.find(key);
   if (i == cache.end()) {
      err_printf("Asked to update replication %s/%s, but not in cache\n", dirname, filename);
      return LDCS_CACHE_FILE_NOT_FOUND;
   };
   debug_printf3("Updating cache of %s/%s with replication information\n", dirname, filename);
   i->second->replication = (bool) replication;
   return LDCS_CACHE_FILE_FOUND;
}

ldcs_cache_result_t ldcs_cache_updateErrcode(char *filename, char *dirname, int errcode)
{
   file_location_t key(dirname, filename);
   cache_t::iterator i = cache.find(key);
   if (i == cache.end()) {
      err_printf("Asked to update %s/%s errcode, but wasn't found in cache\n",
                 dirname, filename);
      return LDCS_CACHE_FILE_NOT_FOUND;
   }
   entry_t *e = i->second;
   e->errcode = errcode;
   if (errcode) {
      e->file_contents.path = string();
      e->dso_contents.path = string();
   }
   debug_printf3("Update %s/%s errcode to %d\n", dirname, filename, errcode);
   return LDCS_CACHE_FILE_FOUND;
}

ldcs_cache_result_t ldcs_cache_updateBuffer(char *filename, char *dirname,
                                            char *localname,
                                            void *buffer, size_t buffer_size,
                                            int errcode, int is_stripped)
{
   file_location_t key(dirname, filename);
   cache_t::iterator i = cache.find(key);
   if (i == cache.end()) {
      err_printf("Asked to update %s %s/%s, but wasn't found in cache\n",
                    is_stripped ? "dso" : "file", dirname, filename);
      return LDCS_CACHE_FILE_NOT_FOUND;
   }
   entry_t *e = i->second;
   if (errcode) {
      e->dso_contents.reset();
      e->file_contents.reset();
      e->errcode = errcode;
      debug_printf3("Updated cache of %s/%s with errcode %d\n", dirname, filename, errcode);
      return LDCS_CACHE_FILE_FOUND;
   }
   cached_contents_t &contents = is_stripped ? e->dso_contents : e->file_contents;

   if (localname)
      contents.path = string(localname);
   else
      contents.path = string();
   contents.buffer = buffer;
   contents.buffer_size = buffer_size;
   e->errcode = 0;
   debug_printf3("Updated %s cache of %s/%s with new file information (%p and %lu; %s)\n",
                 is_stripped ? "dso" : "file", dirname, filename, buffer, buffer_size, localname);
   return LDCS_CACHE_FILE_FOUND;
}

int ldcs_cache_get_buffer(char *dirname, char *filename, ldcs_hash_fileobj_t objt, void **buffer, size_t *size, char **alias_to)
{
   file_location_t key(dirname, filename);
   cache_t::iterator i = cache.find(key);
   if (i == cache.end()) {
      err_printf("Asked to get %s %s/%s buffer, but wasn't found in cache\n",
                 objt == LDCS_CACHE_FILEOBJ_DSO ? "dso" : (objt == LDCS_CACHE_FILEOBJ_FILE ? "file" : "any"),                 
                 dirname, filename);
      return -1;
   }
   entry_t *e = i->second;
   cached_contents_t *contents = NULL;
   switch (objt) {
      case LDCS_CACHE_FILEOBJ_DSO:
         contents = &(e->dso_contents);
         break;
      case LDCS_CACHE_FILEOBJ_FILE:
         contents = &(e->file_contents);
         break;
      case LDCS_CACHE_FILEOBJ_EITHER:
         if (!e->dso_contents.path.empty())
            contents = &(e->dso_contents);
         else
            contents = &(e->file_contents);
         break;
   }   
   *buffer = contents->buffer;
   *size = contents->buffer_size;
   *alias_to = const_cast<char *>(e->alias.c_str());
   return 0;
}

static void lsAndCacheFiles(string dirname, size_t *bytesread) {
   size_t len;
   int result;
   debug_printf3("lsAndCacheFiles for directory %s\n", dirname.c_str());
   
   DIR *d = opendir(dirname.c_str());
   struct dirent *dent = NULL, *entry;
   if (!d) {
      debug_printf3("Could not open directory %s, empty entry added\n", dirname.c_str());
      addDirectory(dirname, false);
      return;
   }
   else {
      addDirectory(dirname, true); 
   }


   len = offsetof(struct dirent, d_name) + pathconf(dirname.c_str(), _PC_NAME_MAX) + 1;
   entry = (struct dirent *) malloc(len);

   for (;;) {
      GCC7_DISABLE_WARNING("-Wdeprecated-declarations");
      result = readdir_r(d, entry, &dent);
      GCC7_ENABLE_WARNING      
      if (result != 0)
         break;
      if (dent == NULL)
         break;
      if (bytesread) *bytesread += sizeof(dent);
      if (dent->d_type != DT_LNK && dent->d_type != DT_REG && dent->d_type != DT_UNKNOWN && dent->d_type != DT_DIR) {
         continue;
      }
      addFile(dirname, dent->d_name);
   }

   closedir(d);

   free(entry);
}

#define INITIAL_BUFFER_SIZE 8192
int ldcs_cache_encodeDirContents(char *dir, char **data, int *len)
{
   char *buffer = NULL;
   size_t buffer_size = 0;
   size_t cur_pos = 0;
   int num_entries = 0;

   file_location_t key(dir);
   cache_t::iterator d = cache.find(key);
   if (d == cache.end()) {
      err_printf("Failed to find directory %s in cache\n", dir);
      return -1;
   }
   entry_t *de = d->second;

   buffer_size = INITIAL_BUFFER_SIZE;
   buffer = (char *) malloc(INITIAL_BUFFER_SIZE);
   int length_dir = strlen(dir) + 1;
   memcpy(buffer + cur_pos, &length_dir, sizeof(int));
   cur_pos += sizeof(int);
   memcpy(buffer + cur_pos, dir, length_dir);
   cur_pos += length_dir;
   assert(cur_pos < INITIAL_BUFFER_SIZE);

   for (list<entry_t*>::iterator i = de->direntries.begin(); i != de->direntries.end(); i++) {
      entry_t *e = *i;
      size_t length_fn = !e->path.filename.empty() ? e->path.filename.length() + 1 : 0;
      size_t space_needed = length_fn + sizeof(int);
      if (cur_pos + space_needed >= buffer_size) {
         while (cur_pos + space_needed >= buffer_size)
            buffer_size = buffer_size*2;
         buffer = reinterpret_cast<char *>(realloc(buffer, buffer_size));
      }

      memcpy(buffer + cur_pos, &length_fn, sizeof(int));
      cur_pos += sizeof(int);
      if (length_fn)
         strncpy(buffer + cur_pos, e->path.filename.c_str(), length_fn);
      cur_pos += length_fn;

      num_entries++;
   }

   debug_printf3("Encoded packet for directory with %d entries: %s\n", num_entries, dir);

   *data = buffer;
   *len = cur_pos;

   return 0;   
}

int ldcs_cache_decodeDirContents(char *buffer, size_t len,
                                 char *dirbuffer, size_t dirbuffer_sz)
{
   size_t pos = 0;
   char str[MAX_PATH_LEN+1];
   string dirname;
   int dn_length;
   int num_entries = 0;

   
   memcpy(&dn_length, buffer + pos, sizeof(int));
   pos += sizeof(int);
   assert(pos <= len);
   assert(dn_length && dn_length < MAX_PATH_LEN+1);
   
   memcpy(str, buffer+pos, dn_length);
   str[MAX_PATH_LEN] = '\0';  
   pos += dn_length;
   assert(pos <= len);
   dirname = str;

   bool dir_has_contents = (pos < len);
   addDirectory(dirname, dir_has_contents);
   
   while (pos < len) {
      int fn_length;
      string filename;
      num_entries++;
      memcpy(&fn_length, buffer + pos, sizeof(int));
      assert(fn_length <= MAX_PATH_LEN+1);
      pos += sizeof(int);
      assert(pos <= len);
      if (!fn_length)
         continue;
      
      memcpy(str, buffer+pos, fn_length);
      str[MAX_PATH_LEN] = '\0';
      filename = str;
      pos += fn_length;
      assert(pos <= len);

      addFile(dirname, filename);
   }

   strncpy(dirbuffer, dirname.c_str(), dirbuffer_sz);
   dirbuffer[dirbuffer_sz - 1] = '\0';

   debug_printf3("Decoded packet for directory with %d entries: %s\n", num_entries, dirbuffer);
   return num_entries;
}

void ldcs_cache_addFileDir(char *dname, char *fname)
{
   string f(fname), d(dname);
   addFile(d, f);
}

char *ldcs_cache_result_to_str(ldcs_cache_result_t res)
{
   const char *result;
   switch (res) {
      case LDCS_CACHE_DIR_PARSED_AND_EXISTS: result = "parsed and exists"; break;
      case LDCS_CACHE_DIR_PARSED_AND_NOT_EXISTS: result = "parsed and doesn't exist"; break;
      case LDCS_CACHE_DIR_NOT_PARSED: result = "not parsed"; break;
      case LDCS_CACHE_FILE_FOUND: result = "file found"; break;
      case LDCS_CACHE_FILE_NOT_FOUND: result = "file not found"; break;
      case LDCS_CACHE_UNKNOWN: result = "uknown"; break;
      default: result = "INVALID STATE"; break;
   }
   return const_cast<char *>(result);
}

static set<string> pickone_cache;

int ldcs_cache_pickone_get(char *key)
{
   string key_s(key);
   if (pickone_cache.find(key_s) != pickone_cache.end())
      return 1;
   else
      return 0;
}

void ldcs_cache_pickone_set(char *key)
{
   string key_s(key);
   pickone_cache.insert(key_s);
}
