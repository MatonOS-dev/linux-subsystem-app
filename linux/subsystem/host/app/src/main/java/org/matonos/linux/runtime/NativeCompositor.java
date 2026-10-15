package org.matonos.linux.runtime;

import android.view.Surface;

/**
 * Per-app Wayland compositor core (JNI).
 *
 * <p>The same core the shared host uses, but loaded and run in the app's own
 * process/UID so the Wayland parser is not a privileged, multi-tenant service.
 * One core instance per process serves exactly this app's session.
 *
 * <p>{@code owner} must expose {@code onNativeToplevel(int,int,int,int)} and
 * {@code onNativeToplevelClosed(int)}, matching the shared host's contract.
 */
public final class NativeCompositor {
    static { System.loadLibrary("maton_compositor"); }

    private NativeCompositor() { }

    public static native boolean nativeStart(String socketName, String runtimeDir, Object owner);
    public static native void nativeStop();
    public static native boolean nativeAddSession(int id, String socketPath);
    public static native boolean nativeXwaylandInit(String socketDir, String xwaylandPath);
    public static native String nativeAddXwayland(int session, int uid);
    public static native void nativeAttach(int id, Surface surface, int width, int height);
    public static native void nativeDetach(int id);
    public static native void nativeResize(int id, int width, int height);
    /** Confirm a window's fullscreen state to the app (after Android applied it). */
    public static native void nativeSetFullscreen(int id, boolean on);
    public static native void nativeKey(int id, int key, int scan, int action, int meta, long timeNs);
    public static native void nativeMotion(int id, float x, float y, float verticalScroll,
                                          float horizontalScroll, int action, int buttons, long timeNs);
    public static native void nativeCaptureLost(int id);
    public static native void nativeRelative(int id, float x, float y, float vs, float hs, int buttons, long timeNs);
    public static native void nativeStopped(int id, boolean stopped);
    public static native void nativeClose(int id);
    public static native void nativeLaunchDemo();
}
