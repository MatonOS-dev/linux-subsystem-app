#define _GNU_SOURCE
/*
 * matonos-app-exec: enter the verified app sandbox domain and exec the
 * payload. bubblewrap runs this as the final command of a sandbox.
 *
 * Under full Treble a coredomain may only take file:entrypoint from
 * system_file_type (domain.te), so the verified app image (an exec_type that
 * must never be mislabelled as system or vendor code) cannot be entered by an
 * exec transition. This launcher therefore:
 *
 *   1. computes the app's MLS level from the UID it actually runs as (the
 *      verified stub UID) with the same algorithm Android uses for app
 *      processes (external/selinux android_seapp.c set_range_from_level
 *      LEVELFROM_ALL via seapp_contexts levelFrom=all); a caller-supplied
 *      MATON_APP_LABEL is only accepted if it matches exactly;
 *   2. mounts the app's code image and optional volume *inside this sandbox's
 *      mount namespace* (the loop devices were dev-bound by bwrap); the code
 *      is never mounted at a shared host path;
 *   3. drops the mount capability, changes to the app domain at that level and
 *      execs the payload with execute_no_trans.
 *
 * The launcher runs in the narrow, MLS-trusted matonos_app_launch domain; the
 * app domain it enters is untrusted so MLS constraints really apply.
 */
#include "../../install/linuxd/MatonMls.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

/* Mount one image with a per-app inode label. The superblock label is always
 * matonos_code_fs; the inode label carries the app's MLS categories. */
static int mount_image(const char* loop, const char* target, const char* fstype,
        int read_only, const char* inode_type, const char* level) {
    if (!loop || !*loop || !target || target[0] != '/') {
        fprintf(stderr, "matonos-app-exec: refusing to mount without a sandbox target\n");
        return -1;
    }
    char context[256];
    int written = snprintf(context, sizeof(context),
            "fscontext=u:object_r:matonos_code_fs:s0,context=u:object_r:%s:%s",
            inode_type, level);
    if (written < 0 || (size_t)written >= sizeof(context)) return -1;
    unsigned long flags = MS_NOSUID | MS_NODEV;
    if (read_only) flags |= MS_RDONLY;
    if (mount(loop, target, fstype, flags, context)) return -1;
    return 0;
}

static int mount_app_images(const char* level) {
    const char* code = getenv("MATON_CODE_LOOP");
    if (code && *code) {
        if (mount_image(code, getenv("MATON_CODE_MOUNT"), "erofs", 1,
                    "matonos_app_code_exec", level)) {
            perror("matonos-app-exec: mount code image");
            return -1;
        }
    }
    const char* volume = getenv("MATON_VOLUME_LOOP");
    if (volume && *volume) {
        if (mount_image(volume, getenv("MATON_VOLUME_MOUNT"), "ext4", 0,
                    "matonos_app_volume_file", level)) {
            perror("matonos-app-exec: mount app volume");
            return -1;
        }
    }
    return 0;
}

/* The payload keeps no mount capability: the verified images are read-only
 * (code) or plain data (volume) and must not change under a running app. */
static void drop_capabilities(void) {
    struct __user_cap_header_struct header = { .version = _LINUX_CAPABILITY_VERSION_3 };
    struct __user_cap_data_struct caps[2] = {{0}};
    (void)syscall(SYS_capset, &header, caps);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "matonos-app-exec: missing command\n");
        return 127;
    }
    uid_t uid = getuid();
    char level[64];
    if (maton_mls_level_from_uid(uid, level, sizeof(level))) {
        fprintf(stderr, "matonos-app-exec: uid %u is not an Android app UID\n", (unsigned)uid);
        return 127;
    }
    char label[128];
    int written = snprintf(label, sizeof(label), "u:r:matonos_flatpak_app:%s", level);
    if (written < 0 || (size_t)written >= sizeof(label)) return 127;
    const char* supplied = getenv("MATON_APP_LABEL");
    if (supplied && strcmp(supplied, label)) {
        fprintf(stderr, "matonos-app-exec: supplied label does not match verified UID\n");
        return 127;
    }
    if (mount_app_images(level)) return 127;
    drop_capabilities();
    int fd = open("/proc/thread-self/attr/current", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("matonos-app-exec: attr/current");
        return 127;
    }
    if (write(fd, label, strlen(label)) != (ssize_t)strlen(label)) {
        perror("matonos-app-exec: setcon");
        close(fd);
        return 127;
    }
    close(fd);
    /* The payload is never accepted as an absolute host path; it is resolved
     * inside the sandbox root that bwrap has already pivoted into. */
    execvp(argv[1], &argv[1]);
    perror("matonos-app-exec: exec payload");
    return 127;
}