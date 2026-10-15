package org.matonos.linux.runtime;

/**
 * In-process per-app session bus (JNI).
 *
 * <p>The broker runs in the owning app's process and UID (loaded as a native
 * library), so it parses untrusted D-Bus in the app's context, never in a
 * privileged one. Replaces the old exec'd {@code matonos-dbus-broker} helper and
 * the system-side {@code PerAppRuntime} ownership of the bus.
 */
public final class NativeBroker {
    static { System.loadLibrary("matonos-dbus-broker"); }

    private NativeBroker() { }

    /**
     * Start the bus and connect it to the in-process portal backend.
     *
     * @param socketPath        the session bus socket path (owned by this app)
     * @param policyPath        the default-deny policy file this app wrote
     * @param portalSocketPath  the portal backend LocalSocket path
     * @param portalSecret      64-char hex capability for the MBP1 handshake
     * @return true if the bus thread started
     */
    public static native boolean nativeStart(String socketPath, String policyPath,
                                             String portalSocketPath, String portalSecret);

    /** Stop the bus thread and wait for the GLib main loop to exit. */
    public static native void nativeStop();
}
