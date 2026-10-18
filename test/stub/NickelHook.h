// Minimal stand-in for NickelHook.h so nickelpages.cc can be tested on a desktop.
#pragma once
#include <cstdio>
#include <stdbool.h>
#define nh_log(fmt, ...) fprintf(stderr, "[NickelBookends] " fmt "\n", ##__VA_ARGS__)
#define nh_symoutptr(x) ((void**)(&(x)))
struct nh_info  { const char *name, *desc, *uninstall_flag, *uninstall_xflag; int failsafe_delay; };
struct nh_hook  { const char *sym, *sym_new, *lib; void **out; const char *desc; bool optional; };
struct nh_dlsym { const char *name; void **out; const char *desc; bool optional; };
static inline bool nh_delete_file(const char*) { return true; }
static inline bool nh_delete_dir(const char*)  { return true; }
#define NickelHook(...) int (*nb_test_init)() = &nb_init;
