/* malloc/free wrapper which tags allocated memory in order
 * to check that it came from this malloc implementation rather
 * than a different one. free() exits if given an untagged pointer */
#include <stddef.h>
#include <unistd.h>

#define TAG 0xabcdef1234567890UL
#define HEADER 16

void *__libc_malloc(size_t n);
void *__libc_calloc(size_t n, size_t m);
void *__libc_realloc(void *p, size_t n);
void __libc_free(void *p);

static void *tag(char *block)
{
   if (!block)
      return NULL;
   *(unsigned long *) block = TAG;
   return block + HEADER;
}

static char *untag(void *p)
{
   char *block = (char *) p - HEADER;
   if (*(unsigned long *) block != TAG) {
       /* We got an untagged pointer */
      _exit(5);
   }
   return block;
}

void *malloc(size_t n)
{
   return tag(__libc_malloc(n + HEADER));
}

void *calloc(size_t n, size_t m)
{
   return tag(__libc_calloc(1, n * m + HEADER));
}

void *realloc(void *p, size_t n)
{
   if (!p)
      return malloc(n);
   return tag(__libc_realloc(untag(p), n + HEADER));
}

void free(void *p)
{
   if (p)
      __libc_free(untag(p));
}
