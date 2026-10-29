package org.matonos.linux;

import android.content.Context;
import android.content.Intent;
import android.os.PowerManager;
import android.view.Surface;
import android.system.Os;
import android.util.Log;

import org.matonos.linux.runtime.NativeCompositor;

import java.io.File;

/**
 * The per-app runtime: the session bus, the portal and the Wayland compositor
 * core all run in the app's own process and UID, using the app's own
 * {@link Context} for platform work. No privileged compositor service parses
 * this app's D-Bus or Wayland.
 *
 * <p>Instantiated by the stub's own service ({@code
 * org.matonos.linux.stub.StubService}).
 */
public final class PerAppRuntime implements AutoCloseable {
    /** The stub drives its windows from these callbacks (JNI). */
    public interface WindowHost {
        void windowOpened(int id, int width, int height);
        void windowClosed(int id);
        boolean openUri(Intent intent);
        void inhibitChanged(boolean active);
    }

    private static final int SESSION = 1;

    private final Context context;
    private final File directory;
    private final SessionBus bus;
    private SessionAudio audio;
    private volatile WindowHost host;
    // Native toplevel events happen once. Keep their dimensions so reopening
    // an Android activity can attach to a window that already exists.
    private final java.util.LinkedHashMap<Integer, int[]> windows = new java.util.LinkedHashMap<>();
    private PowerManager.WakeLock wake;
    private boolean inhibited;

    public PerAppRuntime(Context context, File directory, String ref) throws Exception {
        this.context = context;
        this.directory = directory;
        // Flatpak's stock runner bind-mounts this empty lock file at
        // /run/flatpak/per-app-dirs-ref. Android has no XDG_RUNTIME_DIR
        // per-app tree, so the stub runtime supplies its equivalent.
        FlatpakPerAppDirs.ensureRef(directory, ref);
        this.bus = new SessionBus(context, directory, ref, android.os.Process.myUid(),
                this::inhibit, this::open);
        try {
            if (!NativeCompositor.nativeStart("wayland-0", directory.getAbsolutePath(), this))
                throw new IllegalStateException("Cannot start the compositor core");
            if (!NativeCompositor.nativeAddSession(SESSION, new File(directory, "wayland-0").getAbsolutePath()))
                throw new IllegalStateException("Cannot create the session socket");
            startXwayland();
            try { audio = new SessionAudio(context, directory); }
            catch (Exception | LinkageError error) {
                Log.w("MatonRuntime", "Audio unavailable for this app", error);
            }
        } catch (Exception | Error error) {
            // Stop also removes session sockets and releases the JNI owner.
            try { NativeCompositor.nativeStop(); }
            catch (Throwable cleanup) { error.addSuppressed(cleanup); }
            try { bus.close(); }
            catch (Throwable cleanup) { error.addSuppressed(cleanup); }
            try { if (wake != null && wake.isHeld()) wake.release(); }
            catch (Throwable cleanup) { error.addSuppressed(cleanup); }
            throw error;
        }
    }

    /** X11 windows must use the same core as this app's Android surfaces. */
    private void startXwayland() {
        File alias = new File(directory, "wayland-0-x11");
        try {
            Os.remove(alias.getAbsolutePath());
        } catch (android.system.ErrnoException error) {
            if (error.errno != android.system.OsConstants.ENOENT) {
                Log.w("MatonRuntime", "Cannot remove stale X11 socket alias", error);
                return;
            }
        }
        try {
            File binaries = XwaylandFiles.prepare(context, directory);
            File sockets = new File(directory, "x11");
            if (!sockets.isDirectory() && !sockets.mkdir())
                throw new java.io.IOException("Cannot create X11 socket directory");
            // All runtime sockets stay private to this app UID.
            Os.chmod(sockets.getAbsolutePath(), 0700);
            if (!NativeCompositor.nativeXwaylandInit(sockets.getAbsolutePath(), binaries.getAbsolutePath()))
                throw new java.io.IOException("Cannot initialize Xwayland");
            String display = NativeCompositor.nativeAddXwayland(SESSION, android.os.Process.myUid());
            if (display == null || !display.matches("X[0-9]+"))
                throw new java.io.IOException("Cannot create Xwayland session");
            Os.symlink("x11/" + display, alias.getAbsolutePath());
        } catch (Exception error) {
            Log.w("MatonRuntime", "X11 unavailable for this app", error);
        }
    }

    public File directory() { return directory; }
    public void setFlatpakDirectories(File userDir,File systemDir) {
        bus.setFlatpakDirectories(userDir,systemDir);
    }
    public String waylandSocket() { return new File(directory, "wayland-0").getAbsolutePath(); }
    public synchronized void setWindowHost(WindowHost host, int requestedWindow) {
        this.host = host;
        host.inhibitChanged(inhibited);
        for (java.util.Map.Entry<Integer, int[]> window : windows.entrySet()) {
            if (requestedWindow != 0 && requestedWindow != window.getKey()) continue;
            int[] size = window.getValue();
            host.windowOpened(window.getKey(), size[0], size[1]);
            // A new root activity adopts one existing window. Secondary
            // activities reconnect only to their own saved window ID.
            break;
        }
    }

    public synchronized void clearWindowHost(WindowHost host) {
        if (this.host == host) this.host = null;
    }

    /** Called from native when the app requests a window. */
    @SuppressWarnings("unused") private synchronized void onNativeToplevel(int session, int id, int width, int height) {
        windows.put(id, new int[]{width, height});
        WindowHost h = host;
        if (h != null) h.windowOpened(id, width, height);
        else {
            // No activity is hosting (the app closed its last window and is
            // within its grace period): open a new Android window for it.
            try { org.matonos.linux.stub.StubActivity.openWindow(context, id, width, height); }
            catch (RuntimeException e) { android.util.Log.w("MatonRuntime", "Cannot open window " + id, e); }
        }
    }

    /** Called from native when the app asks for a window state. */
    @SuppressWarnings("unused") private void onNativeWindowState(int id, int state) {
        org.matonos.linux.stub.StubActivity.windowState(id, state);
    }
    public void setFullscreen(int id, boolean on) { NativeCompositor.nativeSetFullscreen(id, on); }

    /** Called from native when the app's window is destroyed. */
    @SuppressWarnings("unused") private synchronized void onNativeToplevelClosed(int id) {
        windows.remove(id);
        WindowHost h = host;
        if (h != null) h.windowClosed(id);
    }

    public synchronized boolean hasWindows() { return !windows.isEmpty(); }

    public void attach(int id, Surface surface, int width, int height) { NativeCompositor.nativeAttach(id, surface, width, height); }
    public void detach(int id) { NativeCompositor.nativeDetach(id); }
    public void resize(int id, int width, int height) { NativeCompositor.nativeResize(id, width, height); }
    public void key(int id, int key, int scan, int action, int meta, long timeNs) { NativeCompositor.nativeKey(id, key, scan, action, meta, timeNs); }
    public void motion(int id, float x, float y, float vs, float hs, int action, int buttons, long timeNs) {
        NativeCompositor.nativeMotion(id, x, y, vs, hs, action, buttons, timeNs);
    }
    public void stopped(int id, boolean stopped) { NativeCompositor.nativeStopped(id, stopped); }
    public void closeWindow(int id) { NativeCompositor.nativeClose(id); }

    private boolean open(Intent intent) {
        WindowHost h = host;
        if (h != null) return h.openUri(intent);
        try {
            context.startActivity(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            return true;
        } catch (RuntimeException error) {
            return false;
        }
    }

    private synchronized void inhibit(boolean active) {
        inhibited = active;
        WindowHost h = host;
        if (h != null) h.inhibitChanged(active);
        if (active) {
            if (wake == null) {
                wake = context.getSystemService(PowerManager.class)
                        .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "MatonOS:PerAppInhibit");
                wake.setReferenceCounted(false);
            }
            if (!wake.isHeld()) wake.acquire();
        } else if (wake != null && wake.isHeld()) {
            wake.release();
        }
    }

    @Override public void close() {
        NativeCompositor.nativeStop();
        new File(directory, "wayland-0-x11").delete();
        if (audio != null) audio.close();
        bus.close();
        if (wake != null && wake.isHeld()) wake.release();
    }
}
