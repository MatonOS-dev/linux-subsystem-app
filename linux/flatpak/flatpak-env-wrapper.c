/*
 * MatonOS launcher for the NDK-built Flatpak CLI. linuxd execs the stable
 * /system_ext/bin/flatpak path; give its GPGME and bwrap subprocesses the
 * image paths they need, then preserve argv[0] and replace this process with
 * the tested CLI binary.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char** argv) {
    (void)argc;
    if (setenv("PATH", "/system_ext/bin:/system/bin:/system/xbin", 1) != 0 ||
        setenv("LD_LIBRARY_PATH", "/system_ext/lib64", 1) != 0 ||
        setenv("FLATPAK_BWRAP", "/system_ext/bin/bwrap", 1) != 0) {
        perror("matonos-flatpak: setting runtime environment failed");
        return 127;
    }
    if (setenv("FLATPAK_REVOKEFS_FUSE", "/system_ext/bin/revokefs-fuse", 1) != 0) {
        perror("matonos-flatpak: setting revokefs path failed");
        return 127;
    }
    execv("/system_ext/bin/matonos-flatpak", argv);
    perror("matonos-flatpak: exec failed");
    return 127;
}
