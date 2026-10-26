package org.matonos.linux.stub;

import java.nio.file.Files;
import java.nio.file.Path;

public final class ExtraDataFilesTest {
    public static void main(String[] args) throws Exception {
        Path temp = Files.createTempDirectory("extra-data-cleanup");
        try {
            Path outside = Files.createDirectories(temp.resolve("runtime"));
            Path marker = Files.writeString(outside.resolve("payload"), "runtime");
            Path root = Files.createDirectories(temp.resolve("install/system/runtime/branch"));
            Files.createSymbolicLink(root.resolve("files"), outside);
            Files.writeString(root.resolve("metadata"), "metadata");
            root.toFile().setWritable(false, false);
            root.getParent().toFile().setWritable(false, false);
            root.getParent().getParent().toFile().setWritable(false, false);
            ExtraDataFiles.deleteTree(temp.resolve("install").toFile());
            if (Files.exists(temp.resolve("install"))) throw new AssertionError("read-only staging remains");
            if (!Files.readString(marker).equals("runtime")) throw new AssertionError("followed runtime symlink");
            ExtraDataFiles.deleteTree(temp.resolve("install").toFile());
            System.out.println("PASS extra-data read-only staging cleanup, external runtime preserved, repeated cleanup");
        } finally { ExtraDataFiles.deleteTree(temp.toFile()); }
    }
}
