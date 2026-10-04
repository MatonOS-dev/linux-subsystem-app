#ifndef MATON_MOUNT_H
#define MATON_MOUNT_H

/*
 * Wire contract between the matonos-bwrap shim (outside the sandbox, but
 * unprivileged: it is a direct parent of bubblewrap and knows the sandbox
 * child pid) and the privileged matonos-mount-helper (the only holder of
 * CAP_SYS_ADMIN in this path). Keep both sides in sync.
 *
 * Rendezvous is a connected AF_UNIX SOCK_SEQPACKET *socketpair* created by the
 * trusted wrapper before the helper and the Flatpak CLI are spawned: the helper
 * inherits one end on a fixed fd, the shim inherits the other (the single
 * deliberately non-CLOEXEC fd). A socketpair is never bound to a name, so no
 * other process - and in particular no Flatpak app sharing the host network
 * namespace - can connect to, address or race it.
 *
 * The shim sends one fixed request carrying the verified app id plus, via
 * SCM_RIGHTS, a pidfd for the sandbox process bubblewrap reported on --info-fd.
 * The helper performs all trust decisions itself: it derives the expected stub
 * uid from its kernel-provided peer credentials (SO_PEERCRED), resolves the
 * pidfd to a pid, independently verifies that process (matonos_bwrap domain,
 * expected uid, descendant of this launch's wrapper, foreign mount namespace)
 * and opens /proc/<pid>/ns/mnt itself. It derives the loop devices from the
 * installer's store attach records and mounts at fixed in-sandbox targets. No
 * capability, loop-device path or target path crosses the boundary in either
 * direction.
 */

#include <stdint.h>

#define MATON_MOUNT_MAGIC 0x544e4f4du /* "MONT" */
#define MATON_MOUNT_VERSION 2u

/* The wrapper leaves the shim's socketpair end inherited on the fd number in
 * this environment variable (Flatpak may forward it as a bundled --setenv).
 * The socket has no name, so the number is only useful to a process that
 * already holds the matching end. */
#define MATON_MOUNT_FD_ENV "MATON_MOUNT_FD"

/* Fixed fd the wrapper hands the helper's socketpair end to. */
#define MATON_MOUNT_HELPER_FD 3

/* Verified stub app id (Flatpak ID). The wrapper sets it for the launch; the
 * shim forwards it in the request and the helper validates it. */
#define MATON_APP_ID_ENV "MATON_APP_ID"

/* Fixed in-sandbox mount targets. Neither is ever taken from argv/env or from
 * the shim. Code is the Flatpak deployment at /app. The writable volume is the
 * app's Flatpak data directory, <app home>/.var/app/<app id>; the home is a
 * function of the verified stub uid, never a caller-supplied string. */
#define MATON_CODE_TARGET "/app"
#define MATON_APP_HOME_ROOT "/data/matonos/linux/apps"
#define MATON_VOLUME_SUBDIR "/.var/app"

/* Installer store layout (keep in sync with install/linuxd/FlatpakStore.c). */
#define MATON_STORE_APPS "/data/matonos/linux/store/apps"

struct maton_mount_request {
    uint32_t magic;
    uint32_t version;
    char     app_id[128];
};

struct maton_mount_reply {
    int32_t status;
    char    error[128];
};

/* Filesystem-object label shared by every store image. */
#define MATON_CODE_FSCONTEXT "u:object_r:matonos_code_fs:s0"

#endif
