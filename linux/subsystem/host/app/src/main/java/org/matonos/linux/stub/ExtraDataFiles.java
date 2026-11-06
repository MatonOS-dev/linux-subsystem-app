package org.matonos.linux.stub;

import java.io.File;
import java.nio.file.Files;

/** Cleanup of app-owned install staging, including its read-only system repo. */
final class ExtraDataFiles {
    static void deleteTree(File file) {
        try { org.matonos.linuxhost.stubgen.PlaceholderCleanup.deleteTree(file); }
        catch (java.io.IOException e) { throw new java.io.UncheckedIOException(e); }
    }
    private ExtraDataFiles() { }
}
