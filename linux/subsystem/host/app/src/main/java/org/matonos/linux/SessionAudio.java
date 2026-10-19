package org.matonos.linux;

import android.content.Context;
import android.system.Os;
import org.matonos.linux.runtime.NativeAudio;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;

/** Owns exactly one audio server, alongside the stub's bus and compositor. */
final class SessionAudio implements AutoCloseable {
    SessionAudio(Context context, File directory) throws Exception {
        File config = new File(directory, "pipewire-config");
        if (!config.isDirectory() && !config.mkdir()) throw new IOException("Cannot create audio config");
        Os.chmod(config.getAbsolutePath(), 0700);
        File pulse = new File(directory, "pulse");
        if (!pulse.isDirectory() && !pulse.mkdir()) throw new IOException("Cannot create Pulse directory");
        Os.chmod(pulse.getAbsolutePath(), 0700);
        // Process death leaves filesystem endpoints behind, but kills all owners.
        for (File socket : new File[]{new File(directory, "pipewire-0"),
                new File(directory, "pipewire-0.lock"), new File(pulse, "native")}) {
            if (socket.exists() && !socket.delete()) throw new IOException("Cannot remove stale audio socket");
        }
        // Stubs run this too: the config ships in the subsystem APK, not the stub's.
        try (var input = context.createPackageContext("org.matonos.linux", 0).getAssets()
                        .open("pipewire/maton.conf");
                var output = new FileOutputStream(new File(config, "maton.conf"))) {
            input.transferTo(output);
        }
        if (!NativeAudio.nativeStart(directory.getAbsolutePath(), config.getAbsolutePath()))
            throw new IOException("Cannot start per-app audio");
        try {
            for (File socket : new File[]{new File(directory, "pipewire-0"), new File(pulse, "native")})
                Os.chmod(socket.getAbsolutePath(), 0600);
        } catch (Exception error) {
            NativeAudio.nativeStop();
            throw error;
        }
    }

    @Override public void close() { NativeAudio.nativeStop(); }
}
