package org.matonos.linux;

import android.content.Context;
import android.system.Os;

import org.matonos.linux.runtime.NativeBroker;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.function.Consumer;

/**
 * One in-process session bus, owned and reaped by the app's own process.
 *
 * <p>The bus is a JNI library loaded into this process (see {@link NativeBroker}),
 * so it runs at the app's UID and never in a privileged process. The portal
 * ({@link JavaPortal}) also lives here. There is no exec'd helper.
 */
final class SessionBus implements AutoCloseable {
    final JavaPortal portals;
    private final File socket, policy;
    private volatile boolean running;

    SessionBus(Context context, File directory, String ref, int appUid,
               Consumer<Boolean> changed, JavaPortal.Launcher launcher) throws Exception {
        if (!ref.matches("app/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+"))
            throw new IllegalArgumentException("Invalid application reference");
        String app = ref.split("/")[1];
        socket = new File(directory, "bus");
        policy = new File(directory, "bus.policy");
        // A stale bus must have died with its host before these are removed.
        for (File file : new File[]{socket, policy})
            if (file.exists() && !file.delete()) throw new IOException("Cannot remove stale bus file");
        try (FileOutputStream out = new FileOutputStream(policy)) {
            // The bus is private to this app: it may register and call any
            // name except the reserved portal/bus services (broker.c).
            out.write(("own " + app + "\nown *\nown org.freedesktop.portal.Flatpak\ntalk *\ntalk org.freedesktop.portal.Flatpak\ntalk org.freedesktop.portal.Desktop\ntalk org.freedesktop.Flatpak\n").getBytes(StandardCharsets.UTF_8));
        }
        Os.chmod(policy.getAbsolutePath(), 0600);
        portals = new JavaPortal(context, directory, changed, launcher);
        String portalSocket = new File(directory, "portal-backend").getAbsolutePath();
        if (!NativeBroker.nativeStart(socket.getAbsolutePath(), policy.getAbsolutePath(), portalSocket, portals.secret())) {
            portals.close();
            throw new IOException("Session broker failed to start");
        }
        running = true;
        // The private bus socket appears after native initialization.
        try {
            for (int i = 0; i < 50 && !socket.exists(); i++) Thread.sleep(100);
        } catch (InterruptedException error) {
            try { close(); } finally { Thread.currentThread().interrupt(); }
            throw error;
        }
        if (!socket.exists()) { close(); throw new IOException("Session broker failed to become ready"); }
    }

    boolean isAlive() { return running; }
    void setFlatpakDirectories(File userDir,File systemDir) {
        portals.setFlatpakDirectories(userDir,systemDir);
    }

    @Override public void close() {
        running = false;
        try { portals.close(); } finally {
            try { NativeBroker.nativeStop(); } finally {
                socket.delete(); policy.delete();
            }
        }
    }
}
