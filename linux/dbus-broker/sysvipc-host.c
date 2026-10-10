/* Per-app SysV IPC supervisor, hosted in the stub next to the session bus.
 *
 * Each Flatpak session child (portal, app payload) installs
 * session_filter_sysvipc and hands its seccomp listener over this socket
 * ("<bus>-sysvipc"). One supervisor serves all of them, so an app's sandboxes
 * share SysV objects and no other app can reach them. Only the app's own UID
 * may connect, and only a seccomp listener is accepted.
 */
#include "sysvipc-host.h"
#include "../sysvipc/sysvipc.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static struct maton_sysvipc *g_supervisor;
static pthread_t g_serve, g_accept;
static int g_listen = -1;
static char *g_path;

static void *serve(void *arg) {
    if (maton_sysvipc_serve(arg, -1)) perror("matonos-sysvipc: serve");
    return NULL;
}

static int is_seccomp_listener(int fd) {
    char path[64], target[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(path, target, sizeof(target) - 1);
    if (n < 0) return 0;
    target[n] = 0;
    return !strcmp(target, "anon_inode:seccomp notify");
}

static int receive_listener(int client) {
    struct ucred cred;
    socklen_t size = sizeof(cred);
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &size) || cred.uid != geteuid()) return -1;
    char byte;
    struct iovec io = { &byte, 1 };
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control;
    struct msghdr msg = { .msg_iov = &io, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes) };
    if (recvmsg(client, &msg, MSG_CMSG_CLOEXEC) != 1 || (msg.msg_flags & MSG_CTRUNC)) return -1;
    struct cmsghdr *rights = CMSG_FIRSTHDR(&msg);
    if (!rights || rights->cmsg_level != SOL_SOCKET || rights->cmsg_type != SCM_RIGHTS ||
            rights->cmsg_len != CMSG_LEN(sizeof(int))) return -1;
    int fd;
    memcpy(&fd, CMSG_DATA(rights), sizeof(fd));
    if (!is_seccomp_listener(fd)) { close(fd); return -1; }
    return fd;
}

static void *accept_loop(void *arg) {
    int listen_fd = (int)(intptr_t)arg;
    for (;;) {
        int client = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            break; /* shutdown() on stop */
        }
        int listener = receive_listener(client);
        close(client);
        if (listener >= 0 && maton_sysvipc_add_listener(g_supervisor, listener)) close(listener);
    }
    return NULL;
}

int maton_sysvipc_host_start(const char *bus_path) {
    if (g_supervisor || !bus_path) return -1;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    size_t length = strlen(bus_path);
    if (length + sizeof("-sysvipc") > sizeof(address.sun_path)) return -1;
    g_path = malloc(length + sizeof("-sysvipc"));
    if (!g_path) return -1;
    memcpy(g_path, bus_path, length);
    memcpy(g_path + length, "-sysvipc", sizeof("-sysvipc"));
    strcpy(address.sun_path, g_path);
    g_supervisor = maton_sysvipc_new();
    if (!g_supervisor) goto fail;
    /* The stub removed stale files before starting the bus (SessionBus). */
    g_listen = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (g_listen < 0 || bind(g_listen, (struct sockaddr *)&address, sizeof(address)) ||
            chmod(g_path, 0600) || listen(g_listen, 8)) goto fail;
    if (pthread_create(&g_serve, NULL, serve, g_supervisor)) goto fail;
    if (pthread_create(&g_accept, NULL, accept_loop, (void *)(intptr_t)g_listen)) {
        maton_sysvipc_stop(g_supervisor); pthread_join(g_serve, NULL); goto fail;
    }
    return 0;
fail:
    perror("matonos-sysvipc: start");
    if (g_listen >= 0) { close(g_listen); unlink(g_path); g_listen = -1; }
    maton_sysvipc_free(g_supervisor); g_supervisor = NULL;
    free(g_path); g_path = NULL;
    return -1;
}

void maton_sysvipc_host_stop(void) {
    if (!g_supervisor) return;
    shutdown(g_listen, SHUT_RDWR);
    pthread_join(g_accept, NULL);
    close(g_listen); g_listen = -1;
    unlink(g_path); free(g_path); g_path = NULL;
    maton_sysvipc_stop(g_supervisor);
    pthread_join(g_serve, NULL);
    maton_sysvipc_free(g_supervisor); g_supervisor = NULL;
}
