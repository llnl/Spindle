#if defined(DEP)
int long_dlopen_dep(void) { return 3; }
#else
int long_dlopen_dep(void);
int long_dlopen_value(void) { return long_dlopen_dep() + 1; }
#endif
