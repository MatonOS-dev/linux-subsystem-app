package org.matonos.systembridge;

import org.matonos.systembridge.ILinuxdListener;
import org.matonos.systembridge.ILinuxdIdentity;

interface ILinuxd {
    /** Bridge-only management: metadata, desktop_entry, icon, uninstall,
     * installed_for_uid require {ref, uid}. The bridge
     * verifies the stub UID with PackageManager; listing requires {userId}.
     * Ref-only lookup is not supported. */
    String call(String command, String jsonArgs);
    void subscribe(String topic, ILinuxdListener listener);
    void unsubscribe(String topic, ILinuxdListener listener);
    android.os.ParcelFileDescriptor[] createDnsForwarderSockets(String address, int stubUid);
    /** Returns {ok, argv, env, root, installation, runtimeInstallation}; no spawn or delegated fds. */
    String launchSpec(String ref, String dnsServers, in @nullable android.os.ParcelFileDescriptor x11Directory, @nullable String x11Display, boolean gameControllers, int stubUid, int stubPid, in android.os.ParcelFileDescriptor lifeline);
    /** Called by a generated stub itself ("install me" / "update me") with its
     * own package name: linuxd checks the package belongs to the binder caller
     * and is signed with the stub key via the live System Bridge identity provider,
     * which returns ref and remote from PackageManager metadata. */
    String installSelf(String packageName, String operationId);
    String updateSelf(String packageName, String operationId);
    /** Called by a generated stub itself: linuxd creates the stub's fixed
     * per-app runtime dir /data/matonos/linux/tmp/<uid> (owned by the caller,
     * labelled, no app can squat it) before the stub binds its sockets. */
    String prepareRuntime(String packageName);
    /** Self-only, verified like prepareRuntime. No caller-supplied paths. */
    String downloadsRequest(String packageName);
    String answerDownloads(String packageName, String request, boolean allow);
    /** Bridge-only live PackageManager identity provider. Append-only transaction. */
    void registerIdentity(ILinuxdIdentity identity);
}
