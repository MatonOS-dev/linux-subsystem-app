package org.matonos.systembridge;

/** Self-only capabilities for device-signed generated Linux stubs.
 * No SYSTEM_BRIDGE permission is granted to a stub. Every call authenticates
 * its Binder UID and ref, and rechecks task ownership in the bridge. */
interface ILinuxWindowBridge {
    boolean setClientDecorated(String ref, int taskId, boolean decorated);
    // edges: 0=move, 1=top, 2=bottom, 4=left, 8=right (wlroots).
    boolean beginGeometry(String ref, int taskId, int edges, IBinder lifetime);
    // Cumulative display-pixel displacement from the initiating pointer press.
    boolean updateGeometry(String ref, int taskId, float dx, float dy);
    void endGeometry(String ref, int taskId);
}
