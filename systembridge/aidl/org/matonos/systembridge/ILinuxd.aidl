package org.matonos.systembridge;

import org.matonos.systembridge.ILinuxdIdentity;

interface ILinuxd {
    /** Internal linuxd maintenance and authenticated identity lookup only. */
    String call(String command, String jsonArgs);
    /** Bridge-only live PackageManager identity provider.  */
    void registerIdentity(ILinuxdIdentity identity);
}
