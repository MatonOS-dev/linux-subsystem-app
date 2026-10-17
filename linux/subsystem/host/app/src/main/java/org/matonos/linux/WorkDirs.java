package org.matonos.linux;

import android.content.Context;
import java.io.*;
import java.nio.file.*;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.*;

/** Private operation storage. Unlike cache, Android must not trim these trees. */
public final class WorkDirs {
    private static final Map<Path,Integer> live = new HashMap<>();
    public static File root(Context context) throws IOException {
        File root = new File(context.getNoBackupFilesDir(), "work").getCanonicalFile();
        Files.createDirectories(root.toPath());
        return root;
    }
    public static File directory(Context context, String name) throws IOException {
        if (!name.matches("[a-zA-Z0-9_-]+")) throw new IOException("Invalid work directory");
        File result = new File(root(context), name);
        Files.createDirectories(result.toPath());
        return result;
    }
    /** Check only the final component; the directory is reached through a held fd. */
    public static void checkDnsPath(File root) throws IOException {
        for (String name : new String[]{"forwarder.udp", "forwarder.tcp"}) {
            int bytes = new File(new File(root, "dns"), name).getName()
                    .getBytes(java.nio.charset.StandardCharsets.UTF_8).length;
            if (bytes >= 108) throw new IOException("DNS socket name exceeds AF_UNIX limit: " + bytes + " bytes (maximum 107)");
        }
    }
    public static synchronized AutoCloseable keep(File directory) throws IOException {
        Path path = directory.getCanonicalFile().toPath();
        live.put(path, live.getOrDefault(path, 0) + 1);
        return new AutoCloseable() {
            boolean closed;
            public void close() { synchronized (WorkDirs.class) {
                if (closed) return;
                closed = true;
                int count = live.get(path);
                if (count == 1) live.remove(path); else live.put(path, count - 1);
            }}
        };
    }
    /** Shared lock makes acquiring a lease and sweeping atomic in this process.
     * Never follows symlinks. No clock, size cap or age-based guesses. */
    public static synchronized void sweep(File root) throws IOException {
        if (!Files.exists(root.toPath(), LinkOption.NOFOLLOW_LINKS)) return;
        try (DirectoryStream<Path> entries = Files.newDirectoryStream(root.toPath())) {
            for (Path entry : entries) {
                Path path = entry.toAbsolutePath().normalize();
                boolean held = live.keySet().stream().anyMatch(p -> path.startsWith(p));
                if (held) continue;
                boolean containsLive = live.keySet().stream().anyMatch(p -> p.startsWith(path));
                if (containsLive && Files.isDirectory(entry, LinkOption.NOFOLLOW_LINKS)) sweep(entry.toFile());
                else remove(entry.toFile());
            }
        }
    }
    public static void require(File tree) throws IOException {
        if (!Files.isDirectory(tree.toPath(), LinkOption.NOFOLLOW_LINKS) || !Files.isReadable(tree.toPath()))
            throw new IOException("Operation directory vanished or is unreadable: " + tree);
        try (DirectoryStream<Path> ignored = Files.newDirectoryStream(tree.toPath())) { }
    }
    public static void remove(File tree) throws IOException {
        if (!Files.exists(tree.toPath(), LinkOption.NOFOLLOW_LINKS)) return;
        Files.walkFileTree(tree.toPath(), new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult visitFile(Path p, BasicFileAttributes a) throws IOException {
                Files.delete(p); return FileVisitResult.CONTINUE;
            }
            @Override public FileVisitResult postVisitDirectory(Path p, IOException error) throws IOException {
                if (error != null) throw error;
                Files.delete(p); return FileVisitResult.CONTINUE;
            }
        });
    }
    private WorkDirs() { }
}
