#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int origin_target();

static struct link_map *find_map(const char *libname)
{
   struct link_map *l;
   for (l = _r_debug.r_map; l; l = l->l_next) {
      if (strstr(l->l_name, libname))
         return l;
   }
   return NULL;
}

static int check_name(const char *libname, const char *expected)
{
   struct link_map *l = find_map(libname);
   if (!l) {
      fprintf(stderr, "%s not in link map\n", libname);
      return 1;
   }
   if (strcmp(l->l_name, expected) != 0) {
      fprintf(stderr, "l_name is %s, expected %s\n", l->l_name, expected);
      return 1;
   }
   return 0;
}

static const char *get_path(const char *var)
{
   const char *path = getenv(var);
   if (path && *path)
      return path;
   fprintf(stderr, "%s is not set\n", var);
   return NULL;
}

int main(void)
{
   void *handle;
   int (*long_dlopen_value)(void);
   int value;
   const char *needed_path = get_path("LONG_NEEDED_PATH");
   const char *dlopen_path = get_path("LONG_DLOPEN_PATH");
   const char *dlopen_needed_path = get_path("LONG_DLOPEN_NEEDED_PATH");

   if (!needed_path || !dlopen_path || !dlopen_needed_path)
      return 1;

   value = origin_target();
   if (value != 2) {
      fprintf(stderr, "origin_target returned wrong value\n");
      return 2;
   }
   
   /* Check that the library this executable is linked against has the
    * original path in the link map's l_name. */
   if (check_name("liborigintarget.so", needed_path))
      return 3;

   /* Open a library which in turn has a long NEEDED path */
   handle = dlopen(dlopen_path, RTLD_NOW);
   if (!handle) {
      fprintf(stderr, "dlopen failed: %s\n", dlerror());
      return 4;
   }

   /* Verify dlopen'ed library is usable. */
   long_dlopen_value = (int (*)(void)) dlsym(handle, "long_dlopen_value");
   if (!long_dlopen_value) {
      fprintf(stderr, "dlsym failed: %s\n", dlerror());
      return 5;
   }
   value = long_dlopen_value();
   if (value != 4) {
      fprintf(stderr, "long_dlopen_value returned %d, expected 4\n", value);
      return 6;
   }

   /* Verify that names have been replaced with original path */
   if (check_name("liblongdlopen.so", dlopen_path))
      return 7;
   if (check_name("liblongdlopendep.so", dlopen_needed_path))
      return 8;

   /* Verify that dlclose works. This will free the memory for l_name. */
   if (dlclose(handle) != 0) {
      fprintf(stderr, "dlclose failed: %s\n", dlerror());
      return 9;
   }

   return 0;
}
