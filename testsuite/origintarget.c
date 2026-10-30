#include <stdio.h>

__thread int tlsvar_originexe = -1;
static void on_load() __attribute__((constructor));
static void on_load() { tlsvar_originexe = 10; }
int get_tlsvar_originexe() { return tlsvar_originexe; }

volatile int is_zero = 0;

int origin_target()
{
   if (is_zero)
      printf("HERE\n");
   return 2;
}

int origintarget_calc()
{
   return origin_target();
}
