package org.matonos.linux.stub;

import org.matonos.linuxhost.stubgen.ObjectPacks;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.util.List;
import java.util.Map;

/**
 * The raw, still-unapplied Flatpak extra downloads of one package.
 *
 * <p>Requirement 1 stops Flatpak applying extra data at deploy time, so a
 * fetched commit's checksum-verified downloads stay in the deployment's
 * {@code files/extra} and are packed with the rest of the package. An
 * {@code apply_extra} needs those inputs writable, but pack mounts are
 * read-only, so the package's own service copies them out of its installed
 * pack carriers (the indexed payload offsets are stable across signing) into
 * a writable directory before running the script.
 *
 * <p>No Android dependency: the caller supplies the package's index and one
 * carrier path per indexed pack. Payload offsets point into the carrier APK
 * itself, and every object is STORED, so the content is a direct slice.
 */
public final class ExtraPayload {
    /** The deployment-relative directory holding the raw downloads. */
    static final String EXTRA = "/extra";
    /** The script Flatpak would have run at deploy time. */
    public static final String APPLY_EXTRA = "/bin/apply_extra";

    private ExtraPayload() { }

    /** True when the indexed tree has any raw extra download. */
    public static boolean hasExtra(byte[] index) throws IOException {
        for (ObjectPacks.Entry entry : ObjectPacks.readIndex(index))
            if (entry.path.equals(EXTRA) || entry.path.startsWith(EXTRA + "/")) return true;
        return false;
    }

    /** True when the indexed tree contains {@code path} as a regular file. */
    public static boolean hasFile(byte[] index, String path) throws IOException {
        for (ObjectPacks.Entry entry : ObjectPacks.readIndex(index))
            if (entry.path.equals(path) && entry.kind.equals("file")) return true;
        return false;
    }

    /**
     * Copies every entry under {@code /extra} into {@code destination},
     * returning false (and touching nothing) when the package has none.
     */
    public static boolean materialize(byte[] index, Map<String, String> carriers, File destination) throws IOException {
        if (!destination.isDirectory() && !destination.mkdirs())
            throw new IOException("Cannot create extra working directory");
        boolean found = false;
        for (ObjectPacks.Entry entry : ObjectPacks.readIndex(index)) {
            String path = entry.path;
            if (!path.equals(EXTRA) && !path.startsWith(EXTRA + "/")) continue;
            found = true;
            String name = path.substring(EXTRA.length());
            if (name.startsWith("/")) name = name.substring(1);
            File target = name.isEmpty() ? destination : new File(destination, name);
            if (entry.kind.equals("dir")) {
                ensureDirectories(destination, parent(name));
                if (!target.isDirectory() && !target.mkdir())
                    throw new IOException("Cannot create extra directory " + name);
            } else if (entry.kind.equals("symlink")) {
                Path link = Path.of(entry.target);
                if (link.isAbsolute() || escapes(link)) throw new IOException("Unsafe extra-data link " + name);
                ensureDirectories(destination, parent(name));
                rejectExisting(target, name);
                Files.createSymbolicLink(target.toPath(), link);
            } else if (entry.kind.equals("file")) {
                if (name.isEmpty()) throw new IOException("Invalid extra-data entry");
                ensureDirectories(destination, parent(name));
                rejectExisting(target, name);
                String carrier = carriers.get(entry.pack);
                if (carrier == null) throw new IOException("Missing extra-data carrier " + entry.pack);
                copy(carrier, entry.offset, entry.length, target, entry.mode);
            } else {
                throw new IOException("Unsupported extra-data entry " + path);
            }
        }
        return found;
    }

    private static void copy(String carrier, long offset, long length, File target, int mode) throws IOException {
        if (offset < 0 || length < 0 || offset > Long.MAX_VALUE - length)
            throw new IOException("Invalid extra-data payload range");
        try (RandomAccessFile in = new RandomAccessFile(carrier, "r")) {
            long size = in.length();
            if (offset + length > size) throw new IOException("Extra-data payload is outside its carrier");
            in.seek(offset);
            try (java.io.OutputStream out = Files.newOutputStream(target.toPath(),
                    java.nio.file.StandardOpenOption.CREATE_NEW)) {
                byte[] buffer = new byte[65536];
                long remaining = length;
                while (remaining > 0) {
                    int read = in.read(buffer, 0, (int) Math.min(remaining, buffer.length));
                    if (read < 0) throw new IOException("Extra-data payload is truncated");
                    out.write(buffer, 0, read);
                    remaining -= read;
                }
            }
        }
        if ((mode & 0111) != 0 && !target.setExecutable(true, true))
            throw new IOException("Cannot mark extra-data file executable");
    }

    private static void ensureDirectories(File destination, String relative) throws IOException {
        if (relative.isEmpty()) return;
        File current = destination;
        for (String part : relative.split("/", -1)) {
            if (part.isEmpty() || part.equals(".") || part.equals(".."))
                throw new IOException("Unsafe extra-data path");
            current = new File(current, part);
            Path path = current.toPath();
            if (Files.isSymbolicLink(path)) throw new IOException("Extra-data path crosses a symlink");
            if (Files.exists(path, LinkOption.NOFOLLOW_LINKS)) {
                if (!Files.isDirectory(path, LinkOption.NOFOLLOW_LINKS))
                    throw new IOException("Extra-data path is not a directory");
            } else if (!current.mkdir()) throw new IOException("Cannot create extra-data directory");
        }
    }

    private static void rejectExisting(File target, String name) throws IOException {
        if (Files.exists(target.toPath(), LinkOption.NOFOLLOW_LINKS))
            throw new IOException("Duplicate extra-data entry " + name);
    }

    private static String parent(String name) {
        int slash = name.lastIndexOf('/');
        return slash < 0 ? "" : name.substring(0, slash);
    }

    private static boolean escapes(Path target) {
        for (Path part : target) if (part.toString().equals("..")) return true;
        return false;
    }
}
