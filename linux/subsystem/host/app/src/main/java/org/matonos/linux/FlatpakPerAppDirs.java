package org.matonos.linux;

import java.io.File;
import java.io.IOException;
import java.nio.file.FileAlreadyExistsException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermissions;

/** Supplies the per-app lock file expected by stock Flatpak's bwrap setup. */
public final class FlatpakPerAppDirs {
    private FlatpakPerAppDirs() {}

    public static File ensureRef(File runtimeDirectory, String appRef) throws IOException {
        if (runtimeDirectory == null || appRef == null) throw new IOException("Missing Flatpak runtime or ref");
        String[] parts = appRef.split("/", -1);
        if (parts.length != 4 || !"app".equals(parts[0]) ||
                !parts[1].matches("[A-Za-z0-9_.-]+") ||
                !parts[2].matches("[A-Za-z0-9_.-]+") ||
                !parts[3].matches("[A-Za-z0-9_.-]+")) {
            throw new IOException("Invalid Flatpak application ref for per-app directory: " + appRef);
        }

        Path appDirectory = runtimeDirectory.toPath().resolve(".flatpak").resolve(parts[1]);
        Files.createDirectories(appDirectory);
        Files.setPosixFilePermissions(appDirectory,
                PosixFilePermissions.fromString("rwx------"));
        Path ref = appDirectory.resolve(".ref");
        try {
            Files.createFile(ref, PosixFilePermissions.asFileAttribute(
                    PosixFilePermissions.fromString("rw-------")));
        } catch (FileAlreadyExistsException alreadyPresent) {
            if (!Files.isRegularFile(ref, java.nio.file.LinkOption.NOFOLLOW_LINKS)) {
                throw new IOException("Flatpak per-app ref is not a regular file: " + ref, alreadyPresent);
            }
        }
        Files.setPosixFilePermissions(ref, PosixFilePermissions.fromString("rw-------"));
        return ref.toFile();
    }
}
