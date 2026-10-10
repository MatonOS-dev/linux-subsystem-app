package org.matonos.systembridge;

import org.matonos.systembridge.ILinuxdListener;

interface ILinuxd {
    String call(String command, String jsonArgs);
    void subscribe(String topic, ILinuxdListener listener);
    void unsubscribe(String topic, ILinuxdListener listener);
    android.os.ParcelFileDescriptor[] createDnsForwarderSockets(String address, int stubUid);
    String launchGraphical(String ref, String dnsServers, in @nullable android.os.ParcelFileDescriptor x11Directory, @nullable String x11Display, boolean gameControllers, int stubUid, int stubPid, in android.os.ParcelFileDescriptor lifeline);
    /** Called by a generated stub itself ("install me" / "update me") with its
     * own package name: linuxd checks the package belongs to the binder caller
     * and is signed with the stub key, then reads ref and remote from it. */
    String installSelf(String packageName, String operationId);
    String updateSelf(String packageName, String operationId);
    /** Called by a generated stub itself: linuxd creates the stub's fixed
     * per-app runtime dir /data/matonos/linux/tmp/<uid> (owned by the caller,
     * labelled, no app can squat it) before the stub binds its sockets. */
    String prepareRuntime(String packageName);
}
