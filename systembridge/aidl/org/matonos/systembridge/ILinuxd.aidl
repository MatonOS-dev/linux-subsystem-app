package org.matonos.systembridge;

import org.matonos.systembridge.ILinuxdListener;
import org.matonos.systembridge.ILinuxdIdentity;

interface ILinuxd {
    /** Bridge-only management: metadata, desktop_entry, icon, uninstall,
     * installed_for_uid require {ref, uid}. The bridge
     * verifies the stub UID with PackageManager; listing requires {userId}.
     * Ref-only lookup is not supported for installed apps. Internal final-stub
     * handoff uses {ref, remote, operationId}: deploy_pending, pending_metadata,
     * pending_desktop, pending_icon, abort_pending, and bind_pending (also
     * assigned uid/packageName, checked against the live PM identity). */
    String call(String command, String jsonArgs);
    void subscribe(String topic, ILinuxdListener listener);
    void unsubscribe(String topic, ILinuxdListener listener);
    /** Bridge-only live PackageManager identity provider.  */
    void registerIdentity(ILinuxdIdentity identity);
}
