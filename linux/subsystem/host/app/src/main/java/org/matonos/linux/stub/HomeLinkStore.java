package org.matonos.linux.stub;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermissions;

/** Enable Linux hard-link semantics for downloaded code and data in HOME. */
final class HomeLinkStore {
    static void prepare(File home) throws IOException {
        Path marker = new File(home, ".v3-store-root").toPath();
        // Repeated launches retain the same store and its inode/count metadata.
        if (!Files.exists(marker, LinkOption.NOFOLLOW_LINKS)) {
            Files.createFile(marker, PosixFilePermissions.asFileAttribute(
                    PosixFilePermissions.fromString("rw-------")));
        }
        if (!Files.isRegularFile(marker, LinkOption.NOFOLLOW_LINKS))
            throw new IOException("Invalid MatonOS home link-store marker: " + marker);
    }
}
