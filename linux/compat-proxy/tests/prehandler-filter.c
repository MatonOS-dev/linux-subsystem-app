/* Run an exec'd loader under the generated Android app syscall allowlist.
 * A ptracer observes SIGSYS and reports the blocked syscall number. */
#define _GNU_SOURCE
#include <asm/unistd.h>
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../build/app-policy.h"

static struct sock_filter filter[5 + 2 * (sizeof(android_allowed) / sizeof(android_allowed[0]))];

static int install_android_filter(void) {
    size_t n = 0;
    filter[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                                offsetof(struct seccomp_data, arch));
    filter[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                               AUDIT_ARCH_X86_64, 1, 0);
    filter[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
                                               SECCOMP_RET_KILL_PROCESS);
    filter[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                                offsetof(struct seccomp_data, nr));
    for (size_t i = 0; i < sizeof(android_allowed) / sizeof(android_allowed[0]); i++) {
        filter[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                                   (uint32_t)android_allowed[i], 0, 1);
        filter[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    }
    filter[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP);
    struct sock_fprog program = { .len = (unsigned short)n, .filter = filter };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return -1;
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s loader [loader args...]\n", argv[0]);
        return 2;
    }
    pid_t child = fork();
    if (child < 0) { perror("fork"); return 2; }
    if (!child) {
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL)) _exit(120);
        raise(SIGSTOP);
        if (install_android_filter()) _exit(121);
        execv(argv[1], argv + 1);
        _exit(122);
    }

    int status;
    if (waitpid(child, &status, 0) != child || !WIFSTOPPED(status)) {
        fprintf(stderr, "prehandler tracer: child did not enter initial stop\n");
        return 2;
    }
    if (ptrace(PTRACE_CONT, child, NULL, NULL)) { perror("PTRACE_CONT"); return 2; }
    for (;;) {
        if (waitpid(child, &status, 0) != child) { perror("waitpid"); return 2; }
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) {
            fprintf(stderr, "prehandler tracer: child terminated by signal %d\n", WTERMSIG(status));
            return 128 + WTERMSIG(status);
        }
        if (!WIFSTOPPED(status)) continue;
        int signal = WSTOPSIG(status);
        if (signal == SIGSYS) {
            siginfo_t info;
            memset(&info, 0, sizeof(info));
            if (ptrace(PTRACE_GETSIGINFO, child, NULL, &info)) { perror("PTRACE_GETSIGINFO"); return 2; }
            fprintf(stderr, "PREHANDLER_SIGSYS syscall=%d arch=0x%x\n", info.si_syscall, info.si_arch);
            ptrace(PTRACE_KILL, child, NULL, NULL);
            waitpid(child, &status, 0);
            return 1;
        }
        /* PTRACE_TRACEME reports exec as a synthetic SIGTRAP. */
        if (signal == SIGTRAP) signal = 0;
        if (ptrace(PTRACE_CONT, child, NULL, (void *)(intptr_t)signal)) { perror("PTRACE_CONT"); return 2; }
    }
}
