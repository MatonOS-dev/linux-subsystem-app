package org.matonos.erofs;

/**
 * Mounts EROFS images embedded in Linux stub and runtime APK splits, read-only,
 * at a global path /mnt/matonos/<owner app uid>/<n>. Mounts reach every app namespace through
 * mount propagation; nothing enters an app namespace.
 */
interface IErofsMounter {
    /**
     * Pin the SHA-256 of the stub signing certificate. Only the Linux subsystem
     * app (org.matonos.linux, signed with the MatonOS release certificate) may
     * call this; a later call replaces the pin.
     */
    void pinStubCertificate(in byte[] sha256);

    /**
     * Mount the EROFS image stored as `entry` (under assets/matonos/, stored
     * uncompressed, 4096-aligned) in `apkPath`, an installed split of the
     * caller or of a runtime package. The caller and the split's package must
     * be signed with the pinned certificate. Mounts are shared between callers;
     * while any process holding `token` is alive the mount stays, after that a
     * periodic sweep unmounts it. Returns the mount path.
     */
    @utf8InCpp String mountSplit(@utf8InCpp String apkPath, @utf8InCpp String entry, IBinder token);
}
