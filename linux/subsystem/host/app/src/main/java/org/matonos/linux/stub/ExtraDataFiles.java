package org.matonos.linux.stub;

import java.io.File;
import java.nio.file.Files;

/** Cleanup of app-owned install staging, including its read-only system repo. */
final class ExtraDataFiles {
    static void deleteTree(File file) {
        // Runtime payload symlinks point outside staging into mounted images.
        if (Files.isSymbolicLink(file.toPath())) { file.delete(); return; }
        if (file.isDirectory()) {
            // prepareRuntimeSystem seals these directories before Flatpak runs.
            // Unlinking children needs write permission on their parent.
            file.setWritable(true, true);
            File[] children = file.listFiles();
            if (children != null) for (File child : children) deleteTree(child);
        }
        file.delete();
    }
    private ExtraDataFiles() { }
}
