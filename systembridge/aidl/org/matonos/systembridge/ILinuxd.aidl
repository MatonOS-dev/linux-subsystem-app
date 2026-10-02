package org.matonos.systembridge;

import org.matonos.systembridge.ILinuxdListener;

interface ILinuxd {
    String call(String command, String jsonArgs);
    void subscribe(String topic, ILinuxdListener listener);
    void unsubscribe(String topic, ILinuxdListener listener);
    String launchGraphical(String ref, in android.os.ParcelFileDescriptor runtimeDirectory, String dnsServers, String x11Display);
}
