package org.matonos.systembridge;

import org.matonos.systembridge.ILinuxdListener;

interface ILinuxd {
    String call(String command, String jsonArgs);
    void subscribe(String topic, ILinuxdListener listener);
    void unsubscribe(String topic, ILinuxdListener listener);
    android.os.ParcelFileDescriptor[] createDnsForwarderSockets(String address, int stubUid);
    String launchGraphical(String ref, in android.os.ParcelFileDescriptor runtimeDirectory, String dnsServers, in @nullable android.os.ParcelFileDescriptor x11Directory, @nullable String x11Display, boolean gameControllers, int stubUid, int stubPid, in android.os.ParcelFileDescriptor lifeline);
}
