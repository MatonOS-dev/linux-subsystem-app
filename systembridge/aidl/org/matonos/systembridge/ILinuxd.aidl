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
    /** Called by a generated stub itself ("install me" / "update me") with its
     * own package name: linuxd checks the package belongs to the binder caller
     * and is signed with the stub key via the live System Bridge identity provider,
     * which returns ref and remote from PackageManager metadata. */
    String installSelf(String packageName, String operationId);
    String updateSelf(String packageName, String operationId);
    /** Bridge-only live PackageManager identity provider.  */
    void registerIdentity(ILinuxdIdentity identity);
}
