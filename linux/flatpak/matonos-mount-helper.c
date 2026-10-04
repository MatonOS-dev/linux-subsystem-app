#define _GNU_SOURCE
/*
 * matonos-mount-helper: privileged side of the r24 "mount from outside"
 * design.
 *
 * Per-app verified code images (erofs, read-only) and optional writable
 * volumes (ext4) are mounted into the app sandbox's mount namespace by *this*
 * process, which lives entirely outside the sandbox and holds the only
 * capability that ever touches a mount: CAP_SYS_ADMIN. No capability, loop
 * device or mount(2) ability is ever handed to the sandbox.
 *
 * Rendezvous is an unnamed socketpair created by the trusted launcher wrapper
 * (see maton-mount.h): the wrapper forks this helper with one end on fd 3 and
 * leaves the other end inherited by the matonos-bwrap shim. A socketpair has
 * no name, so an app that shares the host network namespace cannot address,
 * connect to or race it. This helper serves exactly one request from the shim
 * and exits.
 *
 * The helper trusts nothing the shim sends except the app id. Its input is:
 *   - the app id (validated), forwarded by the shim;
 *   - a pidfd for the sandbox process, passed with SCM_RIGHTS.
 * Everything else it derives itself:
 *   - the expected stub uid from SO_PEERCRED (the shim runs as that uid);
 *   - the per-app MLS level from that uid (MatonMls.h);
 *   - the loop devices from the installer's store attach records;
 *   - the in-sandbox targets (fixed; see maton-mount.h).
 *
 * Before mounting it independently verifies the sandbox process:
 *   - the pidfd resolves to a live pid;
 *   - /proc/<pid>/status shows the expected stub uid;
 *   - /proc/<pid>/attr/current is matonos_bwrap at the expected MLS level;
 *   - its parent chain reaches this launch's wrapper (our own parent);
 *   - /proc/<pid>/ns/mnt exists, is a mount namespace and differs from ours.
 * Only then does it open that namespace itself and attach the mounts. Any
 * failure is fail-closed.
 *
 * The image is instantiated as a detached mount with the new mount API
 * (fsopen/fsconfig/fsmount) *in the host namespace*, where the installer-
 * attached loop device is valid, and is then attached at the sandbox target
 * with move_mount() from a short-lived child that setns()es into the sandbox
 * mount namespace. bubblewrap holds --block-fd until the shim confirms the
 * mounts succeeded, so the payload never runs against an unmounted root.
 *
 * Mount propagation needs no action here: bubblewrap already makes its new
 * mount namespace a slave of the host with
 * mount(NULL, "/", NULL, MS_SLAVE | MS_REC, NULL) before it sets the sandbox
 * up, so every mount made through the namespace fd stays confined to it.
 */
#include "../../install/linuxd/MatonMls.h"
#include "maton-mount.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/mount.h>
#include <linux/nsfs.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MOUNT_ATTR_RDONLY
#define MOUNT_ATTR_RDONLY 0x00000001
#endif
#ifndef MOUNT_ATTR_NOSUID
#define MOUNT_ATTR_NOSUID 0x00000002
#endif
#ifndef MOUNT_ATTR_NODEV
#define MOUNT_ATTR_NODEV 0x00000004
#endif
#ifndef MOVE_MOUNT_F_EMPTY_PATH
#define MOVE_MOUNT_F_EMPTY_PATH 0x00000004
#endif
#ifndef MOVE_MOUNT_T_EMPTY_PATH
#define MOVE_MOUNT_T_EMPTY_PATH 0x00000040
#endif
#ifndef FSOPEN_CLOEXEC
#define FSOPEN_CLOEXEC 0x00000001
#endif
#ifndef FSMOUNT_CLOEXEC
#define FSMOUNT_CLOEXEC 0x00000001
#endif
#ifndef FSCONFIG_SET_STRING
#define FSCONFIG_SET_STRING 1
#endif
#ifndef FSCONFIG_CMD_CREATE
#define FSCONFIG_CMD_CREATE 6
#endif
#ifndef NS_GET_NSTYPE
#define NS_GET_NSTYPE _IO(0xb7, 0x3)
#endif
#ifndef CLONE_NEWNS
#define CLONE_NEWNS 0x00020000
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

typedef struct maton_mount_request mount_request;
typedef struct maton_mount_reply mount_reply;

#define ATTACH_READY  0
#define ATTACH_RETRY  1
#define ATTACH_FAILED 2

/* How long to wait for bubblewrap to finish building the sandbox root. */
#define ATTACH_ATTEMPTS 100
#define ATTACH_DELAY_MS 100

static long sys_fsopen(const char* name, unsigned flags) {
    return syscall(SYS_fsopen, name, flags);
}
static long sys_fsconfig(int fd, unsigned cmd, const char* key, const void* value, int aux) {
    return syscall(SYS_fsconfig, fd, cmd, key, value, aux);
}
static long sys_fsmount(int fd, unsigned flags, unsigned attr) {
    return syscall(SYS_fsmount, fd, flags, attr);
}
static long sys_move_mount(int from_dfd, const char* from_path, int to_dfd,
        const char* to_path, unsigned flags) {
    return syscall(SYS_move_mount, from_dfd, from_path, to_dfd, to_path, flags);
}

/* ---- small helpers -------------------------------------------------------- */

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

/* Open /proc/<pid>/ns/mnt and confirm it is a mount namespace that is not our
 * own. Returns the fd or -1. */
static int open_sandbox_mount_namespace(pid_t pid) {
    char path[64];
    if (snprintf(path, sizeof(path), "/proc/%d/ns/mnt", pid) >= (int)sizeof(path)) return -1;
    int ns_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (ns_fd < 0) return -1;
    if (ioctl(ns_fd, NS_GET_NSTYPE) != CLONE_NEWNS) { close(ns_fd); return -1; }
    struct stat sandbox, self;
    int self_fd = open("/proc/self/ns/mnt", O_RDONLY | O_CLOEXEC);
    if (self_fd < 0) { close(ns_fd); return -1; }
    int same = fstat(ns_fd, &sandbox) == 0 && fstat(self_fd, &self) == 0;
    close(self_fd);
    if (!same || (sandbox.st_dev == self.st_dev && sandbox.st_ino == self.st_ino)) {
        close(ns_fd);
        return -1;
    }
    return ns_fd;
}

/* ---- store attach records ------------------------------------------------ */

/* Derive the loop device for an app image from the installer's attach record.
 * The record holds a /dev/block/loopN path; only that shape is accepted and
 * the device is rebuilt from its digits, never used as an arbitrary path. */
static int read_loop_record(const char* app_id, const char* name, char* loop_out, size_t size) {
    char path[512], record[128];
    int written = snprintf(path, sizeof(path), "%s/%s/%s.loop", MATON_STORE_APPS, app_id, name);
    if (written < 0 || (size_t)written >= sizeof(path)) return -1;
    if (read_text(path, record, sizeof(record)) < 0) return -1;
    chomp(record);
    static const char prefix[] = "/dev/block/loop";
    if (strncmp(record, prefix, sizeof(prefix) - 1)) return -1;
    const char* digits = record + sizeof(prefix) - 1;
    if (!*digits) return -1;
    for (const char* p = digits; *p; p++)
        if (*p < '0' || *p > '9') return -1;
    written = snprintf(loop_out, size, "/dev/block/loop%s", digits);
    if (written < 0 || (size_t)written >= size) return -1;
    return 0;
}

/* The app's Flatpak data directory inside the sandbox (see maton-mount.h). */
static int build_volume_target(uid_t uid, const char* app_id, char* out, size_t size) {
    int written = snprintf(out, size, "%s/%u%s/%s",
            MATON_APP_HOME_ROOT, (unsigned)uid, MATON_VOLUME_SUBDIR, app_id);
    if (written < 0 || (size_t)written >= size) return -1;
    return 0;
}

/* ---- mounting ------------------------------------------------------------ */

/* Instantiate the filesystem as a detached mount in the host namespace. The
 * loop device path is resolved here, before any setns(). */
static int make_detached_mount(const char* fstype, const char* loop, const char* inode_type,
        const char* level, unsigned attr, int* out_mount_fd) {
    int fsfd = (int)sys_fsopen(fstype, FSOPEN_CLOEXEC);
    if (fsfd < 0) return -1;
    char context[300];
    int written = snprintf(context, sizeof(context), "u:object_r:%s:%s", inode_type, level);
    if (written < 0 || (size_t)written >= sizeof(context)) { close(fsfd); return -1; }
    if (sys_fsconfig(fsfd, FSCONFIG_SET_STRING, "source", loop, 0) ||
        sys_fsconfig(fsfd, FSCONFIG_SET_STRING, "fscontext", MATON_CODE_FSCONTEXT, 0) ||
        sys_fsconfig(fsfd, FSCONFIG_SET_STRING, "context", context, 0) ||
        sys_fsconfig(fsfd, FSCONFIG_CMD_CREATE, NULL, NULL, 0)) {
        close(fsfd);
        return -1;
    }
    int mfd = (int)sys_fsmount(fsfd, FSMOUNT_CLOEXEC, attr);
    close(fsfd);
    if (mfd < 0) return -1;
    *out_mount_fd = mfd;
    return 0;
}

/* One attach attempt in a child that setns()es into the sandbox mount
 * namespace. Returns ATTACH_* (never leaves the caller in the sandbox). */
static int attach_once(int mount_fd, int ns_fd, const char* target) {
    pid_t child = fork();
    if (child < 0) return ATTACH_FAILED;
    if (child == 0) {
        if (setns(ns_fd, CLONE_NEWNS)) _exit(20);
        int tfd = open(target, O_PATH | O_DIRECTORY | O_CLOEXEC);
        if (tfd < 0) _exit(errno == ENOENT ? 21 : 22);
        if (sys_move_mount(mount_fd, "", tfd, "",
                    MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_EMPTY_PATH)) {
            int saved = errno;
            close(tfd);
            _exit(saved == ENOENT ? 21 : 23);
        }
        close(tfd);
        _exit(0);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) { }
    if (!WIFEXITED(status)) return ATTACH_FAILED;
    if (WEXITSTATUS(status) == 0) return ATTACH_READY;
    if (WEXITSTATUS(status) == 21) return ATTACH_RETRY;
    return ATTACH_FAILED;
}

/* Wait for bubblewrap to finish sandbox setup, then attach. */
static int attach_with_retry(int mount_fd, int ns_fd, const char* target) {
    for (int attempt = 0; attempt < ATTACH_ATTEMPTS; attempt++) {
        int result = attach_once(mount_fd, ns_fd, target);
        if (result == ATTACH_READY) return 0;
        if (result == ATTACH_FAILED) return -1;
        struct timespec delay = { .tv_sec = 0, .tv_nsec = ATTACH_DELAY_MS * 1000000L };
        nanosleep(&delay, NULL);
    }
    return -1;
}

static int mount_image(const char* fstype, const char* loop, const char* inode_type,
        unsigned attr, const char* level, int ns_fd, const char* target) {
    int mount_fd = -1;
    if (make_detached_mount(fstype, loop, inode_type, level, attr, &mount_fd)) return -1;
    int rc = attach_with_retry(mount_fd, ns_fd, target);
    close(mount_fd);
    return rc;
}

/* ---- request handling ---------------------------------------------------- */

static void set_error(mount_reply* reply, const char* text) {
    reply->status = -1;
    snprintf(reply->error, sizeof(reply->error), "%s", text);
}

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
    int ns_fd = open_sandbox_mount_namespace(sandbox_pid);
    if (ns_fd < 0) {
        set_error(&reply, "cannot open sandbox mount namespace");
        goto done;
    }
    /* Code is mandatory for a verified app: derive its loop device from the
     * installer's attach record. */
    char loop[64];
    if (read_loop_record(request.app_id, "code", loop, sizeof(loop))) {
        set_error(&reply, "no attached code image for app");
        goto have_ns;
    }
    if (mount_image("erofs", loop, "matonos_app_code_exec",
                MOUNT_ATTR_RDONLY | MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV,
                level, ns_fd, MATON_CODE_TARGET)) {
        set_error(&reply, "cannot mount code image");
        goto have_ns;
    }
    /* Optional writable volume; never executable. */
    if (read_loop_record(request.app_id, "vol", loop, sizeof(loop)) == 0) {
        char target[640];
        if (build_volume_target(peer_uid, request.app_id, target, sizeof(target))) {
            set_error(&reply, "invalid volume target");
            goto have_ns;
        }
        if (mount_image("ext4", loop, "matonos_app_volume_file",
                    MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV, level, ns_fd, target)) {
            set_error(&reply, "cannot mount volume image");
            goto have_ns;
        }
    }
    reply.status = 0;
have_ns:
    close(ns_fd);
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
