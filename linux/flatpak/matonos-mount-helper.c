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
 *   - the loop devices from the installer's store attach records, OR
 *     the APK images from the launch record packages file;
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
 *
 * Two image source paths:
 * 1. NEW (APK-carried images): the launch record
 *    (/data/matonos/linux/store/apps/<appid>/packages) names APK files under
 *    /data/app/. Each APK contains an erofs image as a stored, 4096-aligned
 *    zip entry. The helper opens the APK with openat2(RESOLVE_BENEATH),
 *    verifies ownership and label (apk_data_file), parses the zip, uses
 *    LOOP_CONFIGURE on the APK fd to create a loop device with offset and
 *    size limit, then mounts via the new mount API.
 * 2. LEGACY (store attach records): the installer writes
 *    /data/matonos/linux/store/apps/<appid>/{code,vol}.loop containing
 *    "/dev/block/loop<N>" paths.
 *
 * The new path is tried first; the legacy path is the fallback (until linuxd
 * switches fully to APK-carried images).
 */
#include "../../install/linuxd/MatonMls.h"
#include "maton-mount.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/loop.h>
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
#include <sys/xattr.h>
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
#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif

/* ---- APK-image constants ------------------------------------------------- */

/* openat2 resolve flags (linux/openat2.h). */
#ifndef RESOLVE_NO_SYMLINKS
#define RESOLVE_NO_SYMLINKS    1
#endif
#ifndef RESOLVE_NO_MAGICLINKS
#define RESOLVE_NO_MAGICLINKS  2
#endif
#ifndef RESOLVE_BENEATH
#define RESOLVE_BENEATH        4
#endif

/* Root under which APK paths must reside. */
#define APK_ROOT_PATH "/data/app"

/* Launch record package file. */
#define LAUNCH_RECORD_DIR "/data/matonos/linux/store/apps"

/* Expected owner of installed APK files: uid 1000 (system). */
#define APK_EXPECTED_UID 1000u

/* Contract entry names (from apk-images-contract.md). */
#define APK_ENTRY_RUNTIME "matonos/runtime.erofs"
#define APK_ENTRY_CODE    "matonos/code.erofs"
#define APK_ENTRY_EXTRA   "matonos/extra.erofs"

#define PACKAGES_LINE_COUNT_MAX 3
#define PACKAGES_LINE_LENGTH_MAX 640

/* ---- loop device ABI: <linux/loop.h> (LOOP_CTL_GET_FREE, LOOP_CONFIGURE,
 * struct loop_config, LO_FLAGS_*) --------------------------------------- */

/* ---- zip constants ------------------------------------------------------- */

#define ZIP_EOCD_SIG         0x06054b50u
#define ZIP_CENTRAL_SIG      0x02014b50u
#define ZIP_LOCAL_SIG        0x04034b50u
#define ZIP64_EOCD_SIG       0x06064b50u
#define ZIP64_EOCD_LOC_SIG   0x07064b50u
#define ZIP_METHOD_STORED    0
#define ZIP64_LIMIT32        0xFFFFFFFFu
#define ZIP64_LIMIT16        0xFFFFu
#define ALIGN_4096           4096u

/* ---- existing types ------------------------------------------------------ */

typedef struct maton_mount_request mount_request;
typedef struct maton_mount_reply mount_reply;

#define ATTACH_READY  0
#define ATTACH_RETRY  1
#define ATTACH_FAILED 2

/* How long to wait for bubblewrap to finish building the sandbox root. */
#define ATTACH_ATTEMPTS 100
#define ATTACH_DELAY_MS 100

/* ---- syscall wrappers (same as existing) --------------------------------- */

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

/* ---- store attach records (legacy path) ---------------------------------- */

/* Derive the loop device for an app image from the installer's attach record. */
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

/* ---- mounting (shared) --------------------------------------------------- */

/* Instantiate the filesystem as a detached mount in the host namespace. */
static int make_detached_mount(const char* fstype, const char* source,
        const char* inode_type, const char* level, unsigned attr, int* out_mount_fd) {
    int fsfd = (int)sys_fsopen(fstype, FSOPEN_CLOEXEC);
    if (fsfd < 0) return -1;
    char context[300];
    int written = snprintf(context, sizeof(context), "u:object_r:%s:%s", inode_type, level);
    if (written < 0 || (size_t)written >= sizeof(context)) { close(fsfd); return -1; }
    if (sys_fsconfig(fsfd, FSCONFIG_SET_STRING, "source", source, 0) ||
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

static int mount_image(const char* fstype, const char* source,
        const char* inode_type, unsigned attr, const char* level,
        int ns_fd, const char* target) {
    int mount_fd = -1;
    if (make_detached_mount(fstype, source, inode_type, level, attr, &mount_fd)) return -1;
    int rc = attach_with_retry(mount_fd, ns_fd, target);
    close(mount_fd);
    return rc;
}

/* ---- APK verification and zip reader ------------------------------------- */

/* Verify an APK path is safe: starts with /data/app/, no "..", no "//",
 * no control characters (per apk-images-contract.md). */
static int valid_apk_path(const char* path) {
    if (!path) return 0;
    size_t len = strlen(path);
    if (len <= (sizeof(APK_ROOT_PATH)) || len > 512) return 0;
    if (strncmp(path, APK_ROOT_PATH, sizeof(APK_ROOT_PATH) - 1) != 0) return 0;
    if (path[sizeof(APK_ROOT_PATH) - 1] != '/') return 0;
    if (strstr(path, "..")) return 0;
    if (strstr(path, "//")) return 0;
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)path[i] < 0x20 && path[i] != '\t') return 0;
    }
    return 1;
}

/* Open an APK with openat2(RESOLVE_NO_SYMLINKS|RESOLVE_NO_MAGICLINKS|
 * RESOLVE_BENEATH) relative to an O_PATH fd of /data/app. Returns the fd or
 * -1 on error. The path must start with /data/app/ — we strip that prefix and
 * use the relative component under /data/app. */
static int open_apk_secured(const char* path) {
    static int root_fd = -1;
    if (root_fd < 0) {
        root_fd = open(APK_ROOT_PATH, O_PATH | O_DIRECTORY | O_CLOEXEC);
        if (root_fd < 0) return -1;
    }
    /* Strip the /data/app/ prefix. */
    const char* relative = path + sizeof(APK_ROOT_PATH); /* includes the trailing / */
    if (*relative == '\0') return -1;
    /* openat2 with RESOLVE_BENEATH ensures the fd never escapes /data/app. */
    int fd = -1;
#ifdef __linux__
    fd = (int)syscall(SYS_openat2, root_fd, relative,
            O_RDONLY | O_CLOEXEC | O_NOCTTY,
            0,
            RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_BENEATH);
#else
    (void)relative;
    return -1;
#endif
    if (fd < 0) return -1;
    return fd;
}

/* Check that fd refers to a regular file owned by APK_EXPECTED_UID (system),
 * not writable by group/other. */
static int verify_apk_ownership(int fd) {
    struct stat st;
    if (fstat(fd, &st)) return -1;
    /* Must be a regular file. */
    if (!S_ISREG(st.st_mode)) return -1;
    /* Must be owned by APK_EXPECTED_UID (system). */
    if (st.st_uid != APK_EXPECTED_UID) return -1;
    /* Must not be writable by group or other. */
    if (st.st_mode & (S_IWGRP | S_IWOTH)) return -1;
    return 0;
}

/* Check that fd has the SELinux label apk_data_file (read via fgetxattr on
 * security.selinux). Returns 0 on match, -1 on error/mismatch. */
static int verify_apk_selinux_label(int fd) {
    char label[256];
    int got = (int)fgetxattr(fd, "security.selinux", label, sizeof(label) - 1);
    if (got <= 0) return -1;
    label[got] = '\0';
    chomp(label);
    /* Expected: "u:object_r:apk_data_file:s0" */
    if (strcmp(label, "u:object_r:apk_data_file:s0") != 0) return -1;
    return 0;
}

/* ---- minimal bounds-checked ZIP reader ----------------------------------- */

/* Read a little-endian uint16 from buffer at offset. */
static uint16_t read16(const unsigned char* buf, size_t off) {
    return (uint16_t)buf[off] | ((uint16_t)buf[off + 1] << 8);
}
/* Read a little-endian uint32 from buffer at offset. */
static uint32_t read32(const unsigned char* buf, size_t off) {
    return (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
           ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
}
/* Read a little-endian uint64 from buffer at offset. */
static uint64_t read64(const unsigned char* buf, size_t off) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)buf[off + i] << (i * 8);
    return v;
}

/* Parse the extra field to extract zip64 values when the standard fields are
 * 0xFFFF/0xFFFFFFFF. Returns 0 on success, -1 on parse error. */
static int parse_zip64_extra(const unsigned char* extra, uint16_t extra_len,
        uint64_t* comp_size, uint64_t* uncomp_size, uint64_t* local_off) {
    uint16_t pos = 0;
    while (pos + 4 <= extra_len) {
        uint16_t id = read16(extra, pos);
        uint16_t size = read16(extra, pos + 2);
        if (pos + 4 + size > extra_len) return -1;
        if (id == 0x0001) { /* Zip64 extended info */
            uint16_t off = 0;
            if (*uncomp_size == ZIP64_LIMIT32) {
                if (off + 8 > size) return -1;
                *uncomp_size = read64(extra, pos + 4 + off);
                off += 8;
            }
            if (*comp_size == ZIP64_LIMIT32) {
                if (off + 8 > size) return -1;
                *comp_size = read64(extra, pos + 4 + off);
                off += 8;
            }
            if (*local_off == ZIP64_LIMIT32) {
                if (off + 8 > size) return -1;
                *local_off = read64(extra, pos + 4 + off);
                off += 8;
            }
            return 0; /* Found the zip64 block; we're done. */
        }
        pos += 4 + size;
    }
    return 0; /* No zip64 block present; values are the 0xFFFF originals. */
}

/* Find an entry in the zip by name. Returns 0 on success, -1 on error/not found.
 * The fd is pread from, so it can be a plain file fd (no need to seek). */
static int find_zip_entry(int fd, const char* entry_name,
        uint64_t* out_data_offset, uint64_t* out_data_size) {
    size_t name_len = strlen(entry_name);
    if (name_len == 0 || name_len > 255) return -1;

    /* Get file size. */
    struct stat st;
    if (fstat(fd, &st)) return -1;
    uint64_t file_size = (uint64_t)st.st_size;
    if (file_size < 22) return -1; /* Too small for even an empty EOCD. */

    /* ---- find EOCD ------------------------------------------------------- */
    /* Search backwards from the end for the EOCD signature (max comment
     * length = 65535, so search at most 65535 + 22 = 65557 bytes). */
    size_t search_start = file_size > 65557 ? file_size - 65557 : 0;
    uint64_t eocd_offset = 0;
    unsigned char buf[65557];
    size_t read_len = (size_t)(file_size - search_start);
    if (read_len > sizeof(buf)) read_len = sizeof(buf);
    if (pread(fd, buf, read_len, (off_t)search_start) != (ssize_t)read_len) return -1;

    int found_eocd = 0;
    for (size_t i = read_len; i >= 22; i--) {
        if (read32(buf, i - 22) == ZIP_EOCD_SIG) {
            eocd_offset = search_start + i - 22;
            found_eocd = 1;
            break;
        }
    }
    if (!found_eocd) return -1;

    /* Parse EOCD (22 bytes fixed). */
    uint16_t cd_entries = read16(buf, (size_t)(eocd_offset - search_start) + 10);
    uint32_t cd_size = read32(buf, (size_t)(eocd_offset - search_start) + 12);
    uint64_t cd_offset = read32(buf, (size_t)(eocd_offset - search_start) + 16);
    uint16_t comment_len = read16(buf, (size_t)(eocd_offset - search_start) + 20);

    /* Check for Zip64 EOCD locator (immediately before EOCD when comment is 0). */
    uint64_t cd_offset_real = cd_offset;
    uint64_t cd_size_real = cd_size;
    uint64_t cd_entries_real = cd_entries;
    if (comment_len == 0 && eocd_offset >= 20) {
        /* Zip64 EOCD locator sits at eocd_offset - 20. */
        unsigned char loc_buf[20];
        if (pread(fd, loc_buf, 20, (off_t)(eocd_offset - 20)) == 20 &&
                read32(loc_buf, 0) == ZIP64_EOCD_LOC_SIG) {
            uint64_t zip64_eocd_off = read64(loc_buf, 8);
            /* Read Zip64 EOCD record (minimum 56 bytes). */
            if (zip64_eocd_off + 56 <= file_size) {
                unsigned char z64[56];
                if (pread(fd, z64, 56, (off_t)zip64_eocd_off) == 56 &&
                        read32(z64, 0) == ZIP64_EOCD_SIG) {
                    cd_entries_real = read64(z64, 24);
                    cd_size_real = read64(z64, 40);
                    cd_offset_real = read64(z64, 48);
                }
            }
        }
    }

    if (cd_entries_real == 0 || cd_offset_real >= file_size) return -1;
    if (cd_size_real == 0 || cd_offset_real + cd_size_real > file_size) return -1;

    /* ---- scan central directory ------------------------------------------ */

    /* Read central directory. */
    size_t cd_read_len = (size_t)cd_size_real;
    if (cd_read_len > 1024 * 1024) return -1; /* Sanity: >1MB CD for one entry? */
    unsigned char* cd = (unsigned char*)malloc(cd_read_len);
    if (!cd) return -1;
    if (pread(fd, cd, cd_read_len, (off_t)cd_offset_real) != (ssize_t)cd_read_len) {
        free(cd);
        return -1;
    }

    uint64_t pos = 0;
    uint64_t matched_local_offset = 0;
    uint64_t matched_comp_size = 0;
    uint64_t matched_uncomp_size = 0;
    uint16_t matched_method = 0;
    uint16_t matched_flags = 0;
    int found = 0;

    while (pos + 46 <= cd_read_len) {
        if (read32(cd, pos) != ZIP_CENTRAL_SIG) break; /* Bad entry, stop. */
        uint16_t method = read16(cd, pos + 10);
        uint32_t comp_size32 = read32(cd, pos + 20);
        uint32_t uncomp_size32 = read32(cd, pos + 24);
        uint16_t fn_len = read16(cd, pos + 28);
        uint16_t extra_len = read16(cd, pos + 30);
        uint16_t cmt_len = read16(cd, pos + 32);
        uint32_t local_off32 = read32(cd, pos + 42);
        uint16_t flags = read16(cd, pos + 8);

        if ((uint64_t)pos + 46 + (uint64_t)fn_len + (uint64_t)extra_len +
                (uint64_t)cmt_len > cd_read_len) break;

        /* Read filename. */
        char filename[256];
        size_t fname_len = fn_len < 255 ? fn_len : 255;
        memcpy(filename, cd + pos + 46, fname_len);
        filename[fname_len] = '\0';

        /* Check if this is the entry we're looking for. */
        if (fn_len == name_len && memcmp(cd + pos + 46, entry_name, name_len) == 0) {
            /* The contract allows exactly one image entry: a duplicate name
             * could make us mount different bytes than another zip reader. */
            if (found) { free(cd); return -1; }
            /* Found candidate. Parse zip64 if needed. */
            uint64_t comp_size = comp_size32;
            uint64_t uncomp_size = uncomp_size32;
            uint64_t local_off = local_off32;

            if (comp_size == ZIP64_LIMIT32 || uncomp_size == ZIP64_LIMIT32 ||
                    local_off == ZIP64_LIMIT32) {
                if (parse_zip64_extra(cd + pos + 46 + fn_len, extra_len,
                            &comp_size, &uncomp_size, &local_off)) {
                    free(cd);
                    return -1;
                }
            }

            /* Check bounds: everything must fit in the file. */
            if (local_off >= file_size) { free(cd); return -1; }

            /* Go read the local file header to get the actual data offset. */
            unsigned char local[30];
            if (pread(fd, local, 30, (off_t)local_off) != 30) { free(cd); return -1; }
            if (read32(local, 0) != ZIP_LOCAL_SIG) { free(cd); return -1; }

            uint16_t local_fn_len = read16(local, 26);
            uint16_t local_extra_len = read16(local, 28);

            /* Verify local header filename matches (sanity). */
            if (local_fn_len != fn_len) { free(cd); return -1; }
            unsigned char local_fn[256];
            if ((uint64_t)local_fn_len > 255) { free(cd); return -1; }
            if (pread(fd, local_fn, local_fn_len, (off_t)(local_off + 30)) !=
                    (ssize_t)local_fn_len) { free(cd); return -1; }
            if (memcmp(local_fn, cd + pos + 46, local_fn_len) != 0) { free(cd); return -1; }

            /* Compute data offset: after local file header + filename + extra. */
            uint64_t data_offset = local_off + 30 + (uint64_t)local_fn_len +
                                   (uint64_t)local_extra_len;

            /* Verify data offset is 4096-aligned (contract requirement). */
            if (data_offset % ALIGN_4096 != 0) { free(cd); return -1; }

            /* Verify no data descriptor (bit 3 of flags must NOT be set). */
            if (flags & 0x08) { free(cd); return -1; }

            /* Verify compressed size from CD matches local header. */
            uint32_t local_comp_size32 = read32(local, 18);
            /* If local header has 0xFFFFFFFF and CD has zip64, local might too.
             * But per the contract the sizes in CD == local header, and for
             * stored entries both are the same and valid. We check the simpler
             * case: if CD has real values, local must match (or both 0xFFFFFFFF). */
            if (local_comp_size32 != (uint32_t)comp_size &&
                    local_comp_size32 != ZIP64_LIMIT32) {
                free(cd);
                return -1;
            }

            /* For stored entries, verify compressed_size == uncompressed_size
             * and the data at data_offset + comp_size fits in the file. */
            if (method != ZIP_METHOD_STORED) { free(cd); return -1; }
            if (comp_size != uncomp_size) { free(cd); return -1; }
            if (data_offset + comp_size > file_size) { free(cd); return -1; }

            matched_local_offset = data_offset;
            matched_comp_size = comp_size;
            matched_uncomp_size = uncomp_size;
            matched_method = method;
            matched_flags = flags;
            found = 1;
            /* Keep scanning so a duplicate entry is rejected. */
        }

        pos += 46 + (uint64_t)fn_len + (uint64_t)extra_len + (uint64_t)cmt_len;
    }
    free(cd);

    if (!found) return -1;
    if (matched_method != ZIP_METHOD_STORED) return -1;
    if (matched_flags & 0x08) return -1; /* No data descriptor. */
    if (matched_comp_size != matched_uncomp_size) return -1;

    *out_data_offset = matched_local_offset;
    *out_data_size = matched_comp_size;
    return 0;
}

/* ---- loop device setup from fd ------------------------------------------- */

/* Allocate a free loop device, configure it from the APK fd with the given
 * offset and sizelimit. Returns the loop device path (e.g. "/dev/loop7") or
 * NULL on error. The caller must close the returned loop device fd and call
 * loop_detach() to release it. */
static int setup_loop_from_fd(int apk_fd, uint64_t offset, uint64_t size,
        char* loop_path, size_t loop_path_size) {
    /* Get a free loop device number. Android's ueventd creates the nodes
     * under /dev/block (as FlatpakStore.c uses); another process can take the
     * device between GET_FREE and CONFIGURE (EBUSY), so retry a few times. */
    char path[64];
    int loop_fd = -1;
    for (int attempt = 0; attempt < 8 && loop_fd < 0; attempt++) {
        int ctl_fd = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
        if (ctl_fd < 0) return -1;
        int loop_num = ioctl(ctl_fd, LOOP_CTL_GET_FREE);
        close(ctl_fd);
        if (loop_num < 0) return -1;

        int written = snprintf(path, sizeof(path), "/dev/block/loop%d", loop_num);
        if (written < 0 || (size_t)written >= sizeof(path)) return -1;

        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) return -1;

        struct loop_config config;
        memset(&config, 0, sizeof(config));
        config.fd = (uint32_t)apk_fd;
        config.info.lo_offset = offset;
        config.info.lo_sizelimit = size;
        config.info.lo_flags = LO_FLAGS_READ_ONLY | LO_FLAGS_AUTOCLEAR;

        if (ioctl(fd, LOOP_CONFIGURE, &config) == 0) {
            loop_fd = fd;
            break;
        }
        int saved_errno = errno;
        close(fd);
        if (saved_errno != EBUSY) { errno = saved_errno; return -1; }
    }
    if (loop_fd < 0) return -1;

    /* Copy the path for the caller. */
    int written = snprintf(loop_path, loop_path_size, "%s", path);
    if (written < 0 || (size_t)written >= loop_path_size) {
        close(loop_fd);
        return -1;
    }

    /* Keep loop_fd open; the caller must close it when done. The AUTOCLEAR
     * flag ensures the loop device is released when the last fd is closed. */
    return loop_fd;
}

/* ---- launch record parser ------------------------------------------------ */

/* Parse the packages file for an app. The file format (per contract):
 *   code    <absolute apk path>
 *   extra   <absolute apk path>   (optional)
 *   runtime <absolute apk path>
 * Lines are max 3; paths must start with /data/app/.
 * Returns the number of valid lines read, or -1 on rejection. */
#define PACKAGES_MAX_LINES 3

typedef struct {
    char* path;     /* Points to a buffer within the parsed data. */
    int    type;    /* 0=runtime, 1=code, 2=extra */
    char* entry;    /* The zip entry name to look for (from contract). */
    char* target;   /* In-sandbox mount target. */
    char* inode_type; /* SELinux inode label type. */
} package_spec;

static const package_spec package_specs[PACKAGES_MAX_LINES] = {
    { NULL, 0, APK_ENTRY_RUNTIME, "/usr", "matonos_runtime_exec" },
    { NULL, 1, APK_ENTRY_CODE,    "/app", "matonos_app_code_exec" },
    { NULL, 2, APK_ENTRY_EXTRA,   "/app/extra", "matonos_app_code_exec" },
};

static int parse_packages_file(const char* app_id, package_spec* specs) {
    char record_path[640];
    int written = snprintf(record_path, sizeof(record_path),
            "%s/%s/packages", LAUNCH_RECORD_DIR, app_id);
    if (written < 0 || (size_t)written >= sizeof(record_path)) return -1;

    /* Read the file content. */
    char content[PACKAGES_MAX_LINES * PACKAGES_LINE_LENGTH_MAX];
    if (read_text(record_path, content, sizeof(content)) < 0) return -1;

    /* Parse line by line. Each line: "<keyword> <path>" */
    int found = 0;
    char* line = content;
    int seen[PACKAGES_MAX_LINES] = {0, 0, 0};

    while (line && *line && found < PACKAGES_MAX_LINES) {
        /* Find line end. */
        char* nl = strchr(line, '\n');
        char* end = nl ? nl : line + strlen(line);
        size_t line_len = (size_t)(end - line);
        if (line_len == 0 && !nl) break;      /* end of file */
        if (line_len == 0 || line_len > 600) goto reject;
        /* Skip leading whitespace. */
        char* start = line;
        while (start < end && (*start == ' ' || *start == '\t')) start++;
        if (start >= end) goto reject;

        /* Find the space between keyword and path. */
        char* space = (char*)memchr(start, ' ', (size_t)(end - start));
        if (!space || space == start) return -1; /* Reject malformed line. */

        /* Extract keyword. */
        char keyword[16];
        size_t kw_len = (size_t)(space - start);
        if (kw_len > 15) return -1;
        memcpy(keyword, start, kw_len);
        keyword[kw_len] = '\0';

        /* Extract path, strip trailing whitespace. */
        char* path_start = space + 1;
        while (path_start < end && (*path_start == ' ' || *path_start == '\t')) path_start++;
        char* path_end = end;
        while (path_end > path_start && (path_end[-1] == ' ' || path_end[-1] == '\t' ||
                path_end[-1] == '\n' || path_end[-1] == '\r')) path_end--;
        if (path_start >= path_end) return -1;
        size_t path_len = (size_t)(path_end - path_start);
        if (path_len > 512) return -1;

        /* Check path validity (must start with /data/app/). */
        char path_buf[640];
        memcpy(path_buf, path_start, path_len);
        path_buf[path_len] = '\0';
        if (!valid_apk_path(path_buf)) return -1;

        /* Match keyword to slot. */
        int idx = -1;
        if (strcmp(keyword, "runtime") == 0) idx = 0;
        else if (strcmp(keyword, "code") == 0) idx = 1;
        else if (strcmp(keyword, "extra") == 0) idx = 2;
        else return -1; /* Unknown keyword. */

        if (seen[idx]) return -1; /* Duplicate. */
        seen[idx] = 1;

        /* Copy the path into the spec. */
        specs[idx].path = (char*)malloc(path_len + 1);
        if (!specs[idx].path) return -1;
        memcpy(specs[idx].path, path_buf, path_len + 1);

        found++;

        line = nl ? nl + 1 : NULL;
    }

    /* Anything after the maximum number of lines rejects the record. */
    if (line && *line) goto reject;

    /* Must have at least runtime and code; extra is optional. */
    if (!seen[0] || !seen[1]) {
        for (int i = 0; i < PACKAGES_MAX_LINES; i++) {
            if (specs[i].path) { free(specs[i].path); specs[i].path = NULL; }
        }
        return -1;
    }

    return found;

reject:
    /* The caller frees any paths already copied into specs. */
    return -1;
}

/* ---- APK-image mount flow ------------------------------------------------ */

/* Mount a single APK-carried image. Opens the APK, finds the zip entry,
 * configures a loop device, and mounts in the sandbox. The loop_fd_out
 * receives the loop device fd (to keep alive during mounting; caller must
 * close). On failure, cleans up loop devices before returning -1. */
static int mount_apk_image(const package_spec* spec, int ns_fd,
        const char* level, int* loop_fd_out) {
    *loop_fd_out = -1;

    /* 1. Open APK with openat2 safety checks. */
    int apk_fd = open_apk_secured(spec->path);
    if (apk_fd < 0) return -1;

    /* 2. Verify ownership and label. */
    if (verify_apk_ownership(apk_fd) || verify_apk_selinux_label(apk_fd)) {
        close(apk_fd);
        return -1;
    }

    /* 3. Find the erofs entry in the zip. */
    uint64_t data_offset = 0, data_size = 0;
    if (find_zip_entry(apk_fd, spec->entry, &data_offset, &data_size)) {
        close(apk_fd);
        return -1;
    }

    /* 4. Set up loop device from the fd with offset and sizelimit. */
    char loop_path[64];
    int loop_fd = setup_loop_from_fd(apk_fd, data_offset, data_size,
            loop_path, sizeof(loop_path));
    close(apk_fd); /* The loop device holds its own reference. */
    if (loop_fd < 0) return -1;

    /* 5. Mount erofs in the sandbox namespace. */
    unsigned attr = MOUNT_ATTR_RDONLY | MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV;
    if (mount_image("erofs", loop_path, spec->inode_type, attr, level,
                ns_fd, spec->target)) {
        close(loop_fd);
        return -1;
    }

    *loop_fd_out = loop_fd;
    return 0;
}

/* ---- request handling ---------------------------------------------------- */

static void set_error(mount_reply* reply, const char* text) {
    reply->status = -1;
    snprintf(reply->error, sizeof(reply->error), "%s", text);
}

/* Try the APK-image path first; fall back to the legacy attach-record path. */
static int serve_attach_images(int ns_fd, const char* app_id,
        const char* level, uid_t peer_uid, mount_reply* reply) {
    (void)peer_uid;
    /* ---- try APK-carried images ----------------------------------------- */
    package_spec specs[PACKAGES_MAX_LINES];
    for (int i = 0; i < PACKAGES_MAX_LINES; i++) {
        specs[i] = package_specs[i];
        specs[i].path = NULL;
    }

    char record_path[640];
    int record_len = snprintf(record_path, sizeof(record_path), "%s/%s/packages",
            LAUNCH_RECORD_DIR, app_id);
    if (record_len < 0 || (size_t)record_len >= sizeof(record_path)) {
        set_error(reply, "launch record path too long");
        return -1;
    }
    int record_exists = access(record_path, F_OK) == 0;
    int have_packages = record_exists ? parse_packages_file(app_id, specs) : -1;
    if (record_exists && have_packages <= 0) {
        /* A launch record that exists but does not parse rejects the launch;
         * never fall back to the legacy store path. */
        for (int i = 0; i < PACKAGES_MAX_LINES; i++) free(specs[i].path);
        set_error(reply, "malformed launch record");
        return -1;
    }
    if (have_packages > 0) {
        /* Mount in order: runtime (idx 0) -> code (idx 1) -> extra (idx 2). */
        int mount_order[3] = {0, 1, 2};
        int loop_fds[3] = {-1, -1, -1};
        int success = 1;

        for (int i = 0; i < 3 && success; i++) {
            int idx = mount_order[i];
            if (!specs[idx].path) continue; /* Optional extra may be absent. */

            /* The shared runtime is s0 (every app level dominates it); app
             * code and extra are at the verified app level. */
            const char* image_level = idx == 0 ? "s0" : level;
            if (mount_apk_image(&specs[idx], ns_fd, image_level, &loop_fds[idx])) {
                set_error(reply, "APK mount failed");
                success = 0;
                break;
            }
        }

        /* Close all loop fds — AUTOCLEAR will release them. */
        for (int i = 0; i < PACKAGES_MAX_LINES; i++) {
            if (loop_fds[i] >= 0) close(loop_fds[i]);
        }

        /* Clean up allocated paths. */
        for (int i = 0; i < PACKAGES_MAX_LINES; i++) {
            if (specs[i].path) free(specs[i].path);
        }

        if (!success) {
            /* If we mounted some, they remain in the sandbox mount namespace.
             * Since the sandbox process hasn't been released (block-fd not yet
             * closed), bwrap will abort the sandbox, and the kernel cleans up
             * the private mount namespace on sandbox exit. */
            return -1;
        }

        reply->status = 0;
        return 0;
    }

    /* Clean up on parse failure (the file may not exist yet for the legacy
     * path, which is fine — just fall through). */
    for (int i = 0; i < PACKAGES_MAX_LINES; i++) {
        if (specs[i].path) { free(specs[i].path); specs[i].path = NULL; }
    }

    /* ---- legacy: store attach records ----------------------------------- */
    /* Code is mandatory. */
    char loop[64];
    if (read_loop_record(app_id, "code", loop, sizeof(loop))) {
        set_error(reply, "no attached code image for app");
        return -1;
    }
    if (mount_image("erofs", loop, "matonos_app_code_exec",
                MOUNT_ATTR_RDONLY | MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV,
                level, ns_fd, MATON_CODE_TARGET)) {
        set_error(reply, "cannot mount code image");
        return -1;
    }
    /* linux-data: writable state is the UID-owned home bind, never vol.img. */

    reply->status = 0;
    return 0;
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

    /* Mount images via the appropriate path. */
    if (serve_attach_images(ns_fd, request.app_id, level, peer_uid, &reply)) {
        /* reply.error was set by serve_attach_images */
        goto have_ns;
    }

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