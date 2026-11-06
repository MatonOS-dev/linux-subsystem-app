package org.matonos.linux.stub;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.FileVisitResult;
import java.nio.file.attribute.BasicFileAttributes;

/** Merges the stub's generated extra-data payload into the full app deployment. */
final class ExtraDataImage {
    static void mergeExtra(File extractedImage, File deployedFiles) throws IOException {
        Path source=extractedImage.toPath().resolve("app/extra");
        Path target=deployedFiles.toPath().resolve("extra");
        if(!Files.exists(source,LinkOption.NOFOLLOW_LINKS))return;
        deleteTree(target);
        Files.move(source,target,java.nio.file.StandardCopyOption.REPLACE_EXISTING);
    }

    private static void deleteTree(Path root) throws IOException {
        org.matonos.linuxhost.stubgen.PlaceholderCleanup.deleteTree(root.toFile());
    }

    private ExtraDataImage() { }
}
