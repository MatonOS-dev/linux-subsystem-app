package org.matonos.linux.stub;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;

/** Reads a command's complete stdout, growing only as data arrives. */
final class FlatpakOutput {
    private FlatpakOutput() { }

    static byte[] readAll(InputStream input) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        long total = 0;
        int count;
        while ((count = input.read(buffer)) != -1) {
            if (count == 0) continue;
            if (total > Integer.MAX_VALUE - (long) count)
                throw new IOException("Flatpak output is too large for an in-memory result");
            output.write(buffer, 0, count);
            total += count;
        }
        return output.toByteArray();
    }
}
