#define _GNU_SOURCE
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

extern long raw_clone_thread(unsigned long, void *, int (*)(void *), void *);
extern char raw_clone_after[];
static _Atomic int completed;
static int work(void *unused) { (void)unused; atomic_store(&completed, 1); return 0; }

int main(void) {
    struct sock_filter code[] = {
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, SYS_clone, 0, 4),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, instruction_pointer)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, (uint32_t)(uintptr_t)raw_clone_after, 0, 1),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = { .len = sizeof(code)/sizeof(code[0]), .filter = code };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
        prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)) return 1;
    size_t size = 1u << 20;
    char *stack = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) return 2;
    unsigned long flags = CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_SYSVSEM;
    if (raw_clone_thread(flags, stack+size, work, NULL) < 0) return 3;
    for (unsigned i=0; i<100000000 && !atomic_load(&completed); i++) __asm__ volatile("pause");
    if (!atomic_load(&completed)) return 4;
    puts("PASS custom-stack clone runs through its exact seccomp gateway");
    return 0;
}
