#ifndef MATON_ADOPT_H
#define MATON_ADOPT_H
#include <stdint.h>
#include <stdatomic.h>
#define ADOPT_SLOTS 1024
struct adopt_thread {
    _Atomic int tid;
    uintptr_t resident_fs, app_fs, resident_sp;
    uintptr_t child_tid;
    unsigned long robust;
    _Atomic int ready;
    long regs[23];
    unsigned long mask;
    _Alignas(64) unsigned char fp[16384];
    uint64_t xfeatures;
};
struct adopt_api {
    unsigned version;
    struct adopt_thread *(*current)(void);
    long (*identity)(uintptr_t *);
};
int adopt_install(void);
_Noreturn void adopt_launch(uintptr_t entry,uintptr_t stack);
struct adopt_thread *adopt_current(void);
#endif
