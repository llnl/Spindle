#include <stdlib.h>

int main(void)
{
   char *r = realpath("/proc/self/exe", NULL);
   if (!r || r[0] != '/')
      return 1;
   free(r);
   return 0;
}
