#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <linux/sched.h>
#include <fcntl.h>
#include <sys/stat.h>
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

    return 0;
}

int main(void) {
    int fds[2];
    /* Zypak's mimic zygote uses a SOCK_STREAM pair and writes its boot and
     * hello strings as separate sendmsg calls. Chromium reads each fixed
     * string with RecvMsgWithPid; exercise that exact framing, including the
     * kernel-added credential cmsg, before testing the later fd-bearing fork
     * request. */
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) < 0) {
        fprintf(stderr, "zygote socketpair: %s\n", strerror(errno));
        return 1;
    }
    int enabled = 1;
    if (setsockopt(fds[0], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) < 0) {
        fprintf(stderr, "zygote SO_PASSCRED: %s\n", strerror(errno));
        return 1;
    }

    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0) {
        close(fds[0]);
        if (reset_child_signal_handlers()) _exit(1);
        static const char boot[] = "ZYGOTE_BOOT";
        static const char hello[] = "ZYGOTE_OK";
        struct iovec iov = {.iov_base = (void *)boot, .iov_len = sizeof(boot)};
        struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
        if (sendmsg(fds[1], &msg, 0) != sizeof(boot)) _exit(2);
        iov.iov_base = (void *)hello;
        iov.iov_len = sizeof(hello);
        if (sendmsg(fds[1], &msg, 0) != sizeof(hello)) _exit(3);

        int passed = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (passed < 0) _exit(4);
        const int reply = 0x5a17;
        iov.iov_base = (void *)&reply;
        iov.iov_len = sizeof(reply);
        char control[CMSG_SPACE(sizeof(int))] = {0};
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &passed, sizeof(passed));
        if (sendmsg(fds[1], &msg, 0) != sizeof(reply)) _exit(5);
        close(passed);
        close(fds[1]);
        _exit(0);
    }

    close(fds[1]);
    static const char boot[] = "ZYGOTE_BOOT";
    static const char hello[] = "ZYGOTE_OK";
    char message[sizeof(boot) + 1] = {0};
    struct iovec iov = {.iov_base = message, .iov_len = sizeof(message)};
    char credential_control[CMSG_SPACE(sizeof(struct ucred))] = {0};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = credential_control,
                         .msg_controllen = sizeof(credential_control)};
    const char *expected[] = {boot, hello};
    const size_t expected_size[] = {sizeof(boot), sizeof(hello)};
    for (size_t i = 0; i < 2; ++i) {
        memset(message, 0, sizeof(message));
        memset(credential_control, 0, sizeof(credential_control));
        msg.msg_controllen = sizeof(credential_control);
        msg.msg_flags = 0;
        iov.iov_len = expected_size[i] + 1;
        ssize_t received = recvmsg(fds[0], &msg, 0);
        struct ucred cred = {.pid = -1};
        int saw_rights = 0;
        for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg;
             cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_CREDENTIALS &&
                cmsg->cmsg_len >= CMSG_LEN(sizeof(cred)))
                memcpy(&cred, CMSG_DATA(cmsg), sizeof(cred));
            else if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
                saw_rights = 1;
        }
        if (received != (ssize_t)expected_size[i] ||
            memcmp(message, expected[i], expected_size[i]) ||
            message[expected_size[i]] != '\0' || cred.pid != child || saw_rights ||
            (msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC))) {
            fprintf(stderr, "zygote %s receive failed: bytes=%zd pid=%d expected_pid=%d flags=%x errno=%s\n",
                    i ? "hello" : "boot", received, cred.pid, child,
                    msg.msg_flags, strerror(errno));
            return 1;
        }
    }

    int reply = 0;
    iov.iov_base = &reply;
    iov.iov_len = sizeof(reply);
    char control[CMSG_SPACE(sizeof(struct ucred)) + CMSG_SPACE(sizeof(int))] = {0};
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    msg.msg_flags = 0;
    ssize_t received = recvmsg(fds[0], &msg, 0);
    if (received != sizeof(reply) || reply != 0x5a17 ||
        (msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC))) {
        fprintf(stderr, "zygote reply receive failed: bytes=%zd reply=%x flags=%x errno=%s\n",
                received, reply, msg.msg_flags, strerror(errno));
        return 1;
    }

    pid_t credential_pid = -1;
    int received_fd = -1;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET) continue;
        if (cmsg->cmsg_type == SCM_CREDENTIALS &&
            cmsg->cmsg_len >= CMSG_LEN(sizeof(struct ucred))) {
            struct ucred cred;
            memcpy(&cred, CMSG_DATA(cmsg), sizeof(cred));
            credential_pid = cred.pid;
        } else if (cmsg->cmsg_type == SCM_RIGHTS &&
                   cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
            memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(received_fd));
        }
    }
    if (credential_pid != child) {
        fprintf(stderr, "zygote SCM_CREDENTIALS pid mismatch: got=%d expected=%d\n",
                credential_pid, child);
        return 1;
    }
    struct stat passed_stat;
    if (received_fd < 0 || fstat(received_fd, &passed_stat) < 0 ||
        !S_ISCHR(passed_stat.st_mode)) {
        fprintf(stderr, "zygote SCM_RIGHTS descriptor invalid: fd=%d errno=%s\n",
                received_fd, strerror(errno));
        return 1;
    }
    close(received_fd);
    close(fds[0]);

    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        fprintf(stderr, "zygote startup child failed (status=%d)\n", status);
        return 1;
    }
    puts("PASS Chromium mimic zygote SOCK_STREAM boot/hello framing, SCM_CREDENTIALS pid, signal reset and SCM_RIGHTS");
    return 0;
}
