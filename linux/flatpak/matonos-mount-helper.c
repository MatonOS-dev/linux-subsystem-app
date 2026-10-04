#define _GNU_SOURCE
/*
 * matonos-mount-helper — vestigial sandbox verification proxy.
 *
 * The layout spike (2026-10-04) proved that stock `flatpak run` binds /app
 * and /usr from read-only Flatpak installations by itself, so this process
 * no longer mounts code images or writable volumes. It still runs as a
 * sandbox identity verification step: the launcher forks it, the matonos-bwrap
 * shim sends a request with the app id and a sandbox pidfd, and this process
 * independently verifies the sandbox process identity before replying SUCCESS.
 *
 * Rendezvous is an unnamed socketpair created by the trusted launcher wrapper
 * (see maton-mount.h): the wrapper forks this helper with one end on fd 3 and
 * leaves the other end inherited by the matonos-bwrap shim. A socketpair has
 * no name, so an app that shares the host network namespace cannot address,
 * connect to or race it. This helper serves exactly one request and exits.
 *
 * The helper trusts nothing the shim sends except the app id. Its input is:
 *   - the app id (validated), forwarded by the shim;
 *   - a pidfd for the sandbox process, passed with SCM_RIGHTS.
 * Everything else it derives itself:
 *   - the expected stub uid from SO_PEERCRED (the shim runs as that uid);
 *   - the per-app MLS level from that uid (MatonMls.h);
 *   - the sandbox process state from /proc.
 *
 * It verifies the sandbox process:
 *   - the pidfd resolves to a live pid;
 *   - /proc/<pid>/status shows the expected stub uid;
 *   - /proc/<pid>/attr/current is matonos_bwrap at the expected MLS level;
 *   - its parent chain reaches this launch's wrapper (our own parent).
 * Any failure is fail-closed (reply error, and the shim kills the sandbox).
 *
 * No code image, volume, loop device or mount(2) operation remains.
 *
 * TODO: if no other purpose emerges for this identity check, the helper
 * binary and its Android.bp module should be removed entirely. The
 * matonos-bwrap shim already holds the sandbox pidfd and could verify the
 * sandbox identity by itself, eliminating this forked process.
 */
#include "../../install/linuxd/MatonMls.h"
#include "maton-mount.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/nsfs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef NS_GET_NSTYPE
#define NS_GET_NSTYPE _IO(0xb7, 0x3)
#endif
#ifndef CLONE_NEWNS
#define CLONE_NEWNS 0x00020000
#endif

/* ---- existing types ------------------------------------------------------ */

typedef struct maton_mount_request mount_request;
typedef struct maton_mount_reply mount_reply;

/* ---- shared helpers ------------------------------------------------------ */

static int read_text(const char* path, char* out, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t got = read(fd, out, size - 1);
    close(fd);
    if (got <= 0) return -1;
    out[got] = '\0';
    return (int)got;
}

/* The pid a pidfd refers to, read from its fdinfo. */
static pid_t pid_from_pidfd(int pidfd) {
    char path[64];
    if (snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", pidfd) >= (int)sizeof(path)) return -1;
    char text[512];
    if (read_text(path, text, sizeof(text)) < 0) return -1;
    char* field = strstr(text, "Pid:");
    if (!field) return -1;
    field += 4;
    while (*field == ' ' || *field == '\t') field++;
    long value = strtol(field, NULL, 10);
    if (value <= 0 || value > 0x7fffffff) return -1;
    return (pid_t)value;
}

/* First field of the "Uid:" line in /proc/<pid>/status. */
static int status_uid(pid_t pid, uid_t* out) {
    char path[64], text[4096];
    if (snprintf(path, sizeof(path), "/proc/%d/status", pid) >= (int)sizeof(path)) return -1;
    if (read_text(path, text, sizeof(text)) < 0) return -1;
    char* field = strstr(text, "\nUid:");
    if (!field) return -1;
    field += 5;
    while (*field == ' ' || *field == '\t') field++;
    long value = strtol(field, NULL, 10);
    if (value < 0) return -1;
    *out = (uid_t)value;
    return 0;
}

static int status_ppid(pid_t pid, pid_t* out) {
    char path[64], text[4096];
    if (snprintf(path, sizeof(path), "/proc/%d/status", pid) >= (int)sizeof(path)) return -1;
    if (read_text(path, text, sizeof(text)) < 0) return -1;
    char* field = strstr(text, "\nPPid:");
    if (!field) return -1;
    field += 6;
    while (*field == ' ' || *field == '\t') field++;
    long value = strtol(field, NULL, 10);
    if (value < 0) return -1;
    *out = (pid_t)value;
    return 0;
}

/* True when pid is ancestor itself or descends from it via the parent chain. */
static int descends_from(pid_t pid, pid_t ancestor) {
    if (ancestor <= 1) return 0;
    for (int depth = 0; depth < 64 && pid > 1; depth++) {
        if (pid == ancestor) return 1;
        pid_t parent = 0;
        if (status_ppid(pid, &parent) || parent <= 0) return 0;
        pid = parent;
    }
    return 0;
}

/* Read a NUL/whitespace/trailing-newline terminated line, stripping trailing
 * whitespace. */
static void chomp(char* text) {
    size_t length = strlen(text);
    while (length && (text[length - 1] == '\n' || text[length - 1] == '\r' ||
                text[length - 1] == ' ' || text[length - 1] == '\t')) {
        text[--length] = '\0';
    }
}

/* Flatpak ids: dotted, at least two components, no leading/trailing dot. */
static int valid_component(const char* value) {
    if (!value || !*value || strlen(value) > 128 || strstr(value, "..")) return 0;
    for (const char* p = value; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                    (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-'))
            return 0;
    return 1;
}

static int valid_app_id(const char* value) {
    if (!valid_component(value)) return 0;
    return strchr(value, '.') != NULL && value[0] != '.' && value[strlen(value) - 1] != '.';
}

/* ---- sandbox identity verification --------------------------------------- */

/* The sandbox process bubblewrap reported must be a live process in the
 * matonos_bwrap domain, at the level derived from the expected stub uid, and a
 * descendant of this launch's wrapper. */
static int verify_sandbox(pid_t pid, uid_t expected_uid, pid_t wrapper_pid,
        const char* level) {
    if (pid <= 1 || pid == getpid()) return -1;
    if (!descends_from(pid, wrapper_pid)) return -1;
    uid_t sandbox_uid = 0;
    if (status_uid(pid, &sandbox_uid)) return -1;
    if (sandbox_uid != expected_uid) return -1;
    char path[64], own[256], expected[256];
    if (snprintf(path, sizeof(path), "/proc/%d/attr/current", pid) >= (int)sizeof(path)) return -1;
    if (read_text(path, own, sizeof(own)) < 0) return -1;
    chomp(own);
    int written = snprintf(expected, sizeof(expected), "u:r:matonos_bwrap:%s", level);
    if (written < 0 || (size_t)written >= sizeof(expected)) return -1;
    if (strcmp(own, expected)) return -1;
    return 0;
}

/* ---- request handling ---------------------------------------------------- */

static void set_error(mount_reply* reply, const char* text) {
    reply->status = -1;
    snprintf(reply->error, sizeof(reply->error), "%s", text);
}

/* No code/volume mounting is performed here. The helper verifies the sandbox
 * identity and replies SUCCESS. */
static int serve_request(int connection, uid_t peer_uid, pid_t wrapper_pid) {
    mount_request request;
    memset(&request, 0, sizeof(request));
    char control[CMSG_SPACE(sizeof(int))];
    struct iovec payload = { .iov_base = &request, .iov_len = sizeof(request) };
    struct msghdr message = { .msg_iov = &payload, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control) };
    mount_reply reply;
    memset(&reply, 0, sizeof(reply));
    int pidfd = -1;
    ssize_t got;
    do {
        got = recvmsg(connection, &message, MSG_CMSG_CLOEXEC);
    } while (got < 0 && errno == EINTR);
    if (got != (ssize_t)sizeof(request)) {
        set_error(&reply, "short request");
        goto done;
    }
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message); cmsg; cmsg = CMSG_NXTHDR(&message, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
                cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
            memcpy(&pidfd, CMSG_DATA(cmsg), sizeof(int));
        }
    }
    if (request.magic != MATON_MOUNT_MAGIC || request.version != MATON_MOUNT_VERSION) {
        set_error(&reply, "bad request");
        goto done;
    }
    request.app_id[sizeof(request.app_id) - 1] = '\0';
    if (!valid_app_id(request.app_id)) {
        set_error(&reply, "invalid app id");
        goto done;
    }
    if (pidfd < 0) {
        set_error(&reply, "missing sandbox pidfd");
        goto done;
    }
    char level[64];
    if (maton_mls_level_from_uid(peer_uid, level, sizeof(level)) ||
            peer_uid < MATON_AID_APP_START || peer_uid >= 20000) {
        set_error(&reply, "peer is not a verified app UID");
        goto done;
    }
    pid_t sandbox_pid = pid_from_pidfd(pidfd);
    if (sandbox_pid <= 0) {
        set_error(&reply, "cannot resolve sandbox pidfd");
        goto done;
    }
    if (verify_sandbox(sandbox_pid, peer_uid, wrapper_pid, level)) {
        set_error(&reply, "sandbox identity verification failed");
        goto done;
    }

    /* Sandbox verified. No mounts to perform. Reply SUCCESS. */
    reply->status = 0;

done:
    if (pidfd >= 0) close(pidfd);
    do {
        got = send(connection, &reply, sizeof(reply), MSG_NOSIGNAL);
    } while (got < 0 && errno == EINTR);
    return reply.status == 0 ? 0 : -1;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    /* The wrapper forks the helper with the socketpair end on this fixed fd
     * (see maton-mount.h). There is no listening socket and no name. */
    int connection = MATON_MOUNT_HELPER_FD;
    if (fcntl(connection, F_GETFD) < 0) {
        fprintf(stderr, "matonos-mount-helper: missing rendezvous fd %d\n", connection);
        return 2;
    }
    /* This launch's wrapper is our direct parent; the sandbox must descend
     * from it. */
    pid_t wrapper_pid = getppid();
    struct ucred cred;
    socklen_t cred_len = sizeof(cred);
    if (getsockopt(connection, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) ||
            cred_len != sizeof(cred)) {
        fprintf(stderr, "matonos-mount-helper: cannot read peer credentials\n");
        return 1;
    }
    int rc = serve_request(connection, cred.uid, wrapper_pid);
    return rc == 0 ? 0 : 1;
}