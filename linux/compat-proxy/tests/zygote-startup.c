#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <linux/sched.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

struct kernel_action {
    void *handler;
    unsigned long flags;
    void *restorer;
    unsigned long mask;
};

/* Chromium resets dispositions with raw rt_sigaction calls before it execs
 * the zygote. Keep the kernel ABI and the stale restorer bits from oldact. */
static int reset_child_signal_handlers(void) {
    /* Flatpak's bwrap gives the app a private network namespace. The proxy
     * models that namespace, so exercise the socketpair in the same state. */
    if (syscall(SYS_unshare, CLONE_NEWNET) < 0) {
        fprintf(stderr, "unshare(CLONE_NEWNET): %s\n", strerror(errno));
        return 1;
    }
    int saw_proxy_restorer = 0;
    for (int sig = 1; sig <= 64; ++sig) {
        struct kernel_action action = {0};
        if (syscall(SYS_rt_sigaction, sig, NULL, &action, 8) < 0) {
            if (errno == EINVAL) break;
            fprintf(stderr, "rt_sigaction(get, %d): %s\n", sig, strerror(errno));
            return 1;
        }
        if ((action.flags & 0x04000000UL) && action.restorer)
            saw_proxy_restorer = 1;
        if (sig == SIGSTOP || sig == SIGKILL) continue;
        action.handler = SIG_DFL;
        action.restorer = NULL;
        if (syscall(SYS_rt_sigaction, sig, &action, NULL, 8) < 0) {
            fprintf(stderr, "rt_sigaction(set, %d): %s flags=%lx\n",
                    sig, strerror(errno), action.flags);
            return 1;
        }
        struct kernel_action installed = {0};
        if (syscall(SYS_rt_sigaction, sig, NULL, &installed, 8) < 0) {
            fprintf(stderr, "rt_sigaction(verify, %d): %s\n", sig, strerror(errno));
            return 1;
        }
        if (installed.handler == SIG_DFL && !installed.restorer &&
            (installed.flags & 0x04000000UL)) {
            fprintf(stderr, "stale SA_RESTORER survived reset for signal %d\n", sig);
            return 1;
        }
    }
    if (!saw_proxy_restorer) {
        fprintf(stderr, "proxy test process had no SA_RESTORER handler to reset\n");
        return 1;
    }

    int fds[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) < 0) {
        fprintf(stderr, "zygote socketpair: %s\n", strerror(errno));
        return 1;
    }
    int enabled = 1;
    int result = setsockopt(fds[0], SOL_SOCKET, SO_PASSCRED,
                            &enabled, sizeof(enabled));
    if (result < 0) fprintf(stderr, "zygote SO_PASSCRED: %s\n", strerror(errno));
    close(fds[0]);
    close(fds[1]);
    return result < 0;
}

int main(void) {
    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0) _exit(reset_child_signal_handlers());

    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        fprintf(stderr, "zygote startup child failed (status=%d)\n", status);
        return 1;
    }
    puts("PASS Chromium zygote signal reset, SOCK_SEQPACKET pair and SO_PASSCRED");
    return 0;
}
