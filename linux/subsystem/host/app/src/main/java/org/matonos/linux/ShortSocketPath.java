package org.matonos.linux;

import android.os.ParcelFileDescriptor;
import android.system.Os;
import android.system.OsConstants;
import java.io.Closeable;
import java.io.File;
import java.io.IOException;

/** A filesystem socket path rooted at a held directory descriptor. */
public final class ShortSocketPath implements Closeable {
    // Linux UAPI flags absent from android.system.OsConstants.
    private static final int O_PATH = 0x200000;
    private static final int O_DIRECTORY = 0x10000;
    private final ParcelFileDescriptor directory;
    private ShortSocketPath(ParcelFileDescriptor directory) { this.directory = directory; }

    public static ShortSocketPath open(File dir) throws Exception {
        java.io.FileDescriptor fd = Os.open(dir.getCanonicalPath(),
                O_PATH | O_DIRECTORY | OsConstants.O_CLOEXEC, 0);
        try { return new ShortSocketPath(ParcelFileDescriptor.dup(fd)); }
        finally { Os.close(fd); }
    }

    public String path(String name) throws IOException {
        if (name == null || name.isEmpty() || name.indexOf('/') >= 0 || name.equals(".") || name.equals(".."))
            throw new IOException("Socket name must be one path component");
        String path = "/proc/self/fd/" + directory.getFd() + "/" + name;
        if (path.getBytes(java.nio.charset.StandardCharsets.UTF_8).length >= 108)
            throw new IOException("Socket name exceeds AF_UNIX sun_path limit");
        return path;
    }

    @Override public void close() throws IOException { directory.close(); }
}
