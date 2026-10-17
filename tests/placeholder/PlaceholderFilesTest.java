import org.matonos.linuxhost.stubgen.PlaceholderFiles;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.util.List;
import java.util.Locale;

/**
 * Host unit tests for {@link PlaceholderFiles}. Every fixture is a real
 * directory tree under a marked store, so the test exercises the same
 * filesystem metadata source (the default {@code unix:} backend) the device
 * helper falls back to, without needing the compatibility proxy.
 */
public final class PlaceholderFilesTest {
    private static int checks;

    private static void check(boolean ok, String message) {
        checks++;
        if (!ok) throw new AssertionError(message);
    }

    private static void equals(Object expected, Object actual, String message) {
        check(expected == null ? actual == null : expected.equals(actual),
                message + " (expected " + expected + ", got " + actual + ")");
    }

    private static void mode(Path path, int value) throws IOException {
        Files.setAttribute(path, "unix:mode", value);
    }

    private static void write(Path path, byte[] data) throws IOException {
        Files.createDirectories(path.getParent());
        Files.write(path, data);
    }

    private static Path marker(Path root, int rootMode) throws IOException {
        Files.createDirectories(root);
        mode(root, rootMode);
        Path store = root.resolve(".v3-inodes");
        Files.createDirectory(store);
        mode(store, 0700);
        write(root.resolve(".v3-store-root"), new byte[0]);
        mode(root.resolve(".v3-store-root"), 0600);
        return store;
    }

    private static String key(Path path) throws IOException {
        long dev = ((Number) Files.getAttribute(path, "unix:dev")).longValue();
        long ino = ((Number) Files.getAttribute(path, "unix:ino")).longValue();
        return String.format(Locale.ROOT, "p-%016x-%016x", dev, ino);
    }

    /** Creates an empty sticky file plus its index entry naming {@code id}. */
    private static Path placeholder(Path store, Path at, String id) throws IOException {
        write(at, new byte[0]);
        mode(at, 01600);
        write(store.resolve(key(at)), fixed(id + "\0"));
        return at;
    }

    private static void index(Path store, Path at, byte[] content) throws IOException {
        write(store.resolve(key(at)), content);
    }

    private static Path sticky(Path at, byte[] content) throws IOException {
        write(at, content);
        mode(at, 01600);
        return at;
    }

    /** Exactly 35 bytes: the id plus NUL, any surplus truncated, zero padded. */
    private static byte[] fixed(String value) {
        byte[] raw = value.getBytes(StandardCharsets.ISO_8859_1);
        byte[] out = new byte[35];
        System.arraycopy(raw, 0, out, 0, Math.min(raw.length, 35));
        return out;
    }

    public static void main(String[] args) throws Exception {
        Path base = Files.createTempDirectory("placeholder-files-unit-");
        try {
            detectionAndContent(base);
            rejectedIndexEntries(base);
            storeAndMarkerEdges(base);
            nestedStores(base);
            walkSkipsStore(base);
            System.out.println("PASS PlaceholderFilesTest (" + checks + " checks)");
        } finally {
            delete(base.toFile());
        }
    }

    /* ---- genuine placeholder and every read shape -------------------------- */

    private static void detectionAndContent(Path base) throws Exception {
        Path root = base.resolve("content");
        Path store = marker(root, 0700);
        String id = "i-" + "a1b2c3d4e5f60718293a4b5c6d7e8f90";
        byte[] payload = "hello placeholder content\n".getBytes(StandardCharsets.UTF_8);
        write(store.resolve(id), payload);
        Path at = placeholder(store, root.resolve("file"), id);

        check(PlaceholderFiles.isPlaceholder(at.toFile()), "genuine placeholder detected");
        equals(store.resolve(id).toFile(), PlaceholderFiles.resolve(at.toFile()), "resolve backing");
        equals(store.resolve(id).toFile(), PlaceholderFiles.resolve(at), "resolve(Path) backing");
        check(java.util.Arrays.equals(payload, PlaceholderFiles.readAllBytes(at.toFile())),
                "readAllBytes returns backing content");
        equals((long) payload.length, PlaceholderFiles.size(at.toFile()), "logical size");

        try (InputStream in = PlaceholderFiles.open(at.toFile())) {
            check(java.util.Arrays.equals(payload, in.readAllBytes()), "open streams backing content");
        }
        try (FileChannel channel = PlaceholderFiles.channel(at.toFile())) {
            java.nio.ByteBuffer buffer = java.nio.ByteBuffer.allocate(payload.length);
            while (buffer.hasRemaining()) channel.read(buffer);
            check(java.util.Arrays.equals(payload, buffer.array()), "channel reads backing content");
        }
        ByteArrayOutputStream copy = new ByteArrayOutputStream();
        equals((long) payload.length, PlaceholderFiles.copy(at.toFile(), copy), "copy byte count");
        check(java.util.Arrays.equals(payload, copy.toByteArray()), "copy bytes");

        // Persistent count sidecar wins over the backing's physical link count.
        write(store.resolve(id + ".count"), little(7));
        equals(7L, PlaceholderFiles.linkCount(at.toFile()), "sidecar link count");
        Files.delete(store.resolve(id + ".count"));
        equals(1L, PlaceholderFiles.linkCount(at.toFile()), "physical link count fallback");

        // Ordinary files are returned unchanged.
        Path ordinary = root.resolve("ordinary");
        write(ordinary, "ordinary".getBytes(StandardCharsets.UTF_8));
        check(!PlaceholderFiles.isPlaceholder(ordinary.toFile()), "ordinary not placeholder");
        equals(ordinary.toFile(), PlaceholderFiles.resolve(ordinary.toFile()), "ordinary resolves to itself");
        check(java.util.Arrays.equals("ordinary".getBytes(StandardCharsets.UTF_8),
                PlaceholderFiles.readAllBytes(ordinary.toFile())), "ordinary content");

        check(!PlaceholderFiles.isPlaceholder(root.resolve("absent").toFile()), "absent path not placeholder");
        check(!PlaceholderFiles.isPlaceholder(root.toFile()), "directory not placeholder");
    }

    /* ---- every reason an index must be rejected ---------------------------- */

    private static void rejectedIndexEntries(Path base) throws Exception {
        Path root = base.resolve("reject");
        Path store = marker(root, 0700);
        String good = "i-" + "0".repeat(32);
        write(store.resolve(good), "backing".getBytes(StandardCharsets.UTF_8));

        Path stray = sticky(root.resolve("stray"), "stray".getBytes(StandardCharsets.UTF_8));
        check(!PlaceholderFiles.isPlaceholder(stray.toFile()), "sticky without index is ordinary");
        check(java.util.Arrays.equals("stray".getBytes(StandardCharsets.UTF_8),
                PlaceholderFiles.readAllBytes(stray.toFile())), "stray keeps its content");

        Path missing = sticky(root.resolve("missing"), new byte[0]);
        check(!PlaceholderFiles.isPlaceholder(missing.toFile()), "missing index rejected");

        Path escape = sticky(root.resolve("escape"), new byte[0]);
        index(store, escape, fixed("../../../../etc/passwd\0"));
        check(!PlaceholderFiles.isPlaceholder(escape.toFile()), "escaping index rejected");

        Path absolute = sticky(root.resolve("absolute"), new byte[0]);
        index(store, absolute, fixed("/etc/passwd\0"));
        check(!PlaceholderFiles.isPlaceholder(absolute.toFile()), "absolute index rejected");

        Path shortId = sticky(root.resolve("short"), new byte[0]);
        index(store, shortId, "i-deadbeef".getBytes(StandardCharsets.ISO_8859_1));
        check(!PlaceholderFiles.isPlaceholder(shortId.toFile()), "short index rejected");

        Path noNul = sticky(root.resolve("nonul"), new byte[0]);
        index(store, noNul, ("i-" + "f".repeat(32)).getBytes(StandardCharsets.ISO_8859_1));
        check(!PlaceholderFiles.isPlaceholder(noNul.toFile()), "unterminated index rejected");

        Path upper = sticky(root.resolve("upper"), new byte[0]);
        index(store, upper, fixed("i-" + "F".repeat(32) + "\0"));
        check(!PlaceholderFiles.isPlaceholder(upper.toFile()), "uppercase hex index rejected");

        Path dangling = sticky(root.resolve("dangling"), new byte[0]);
        index(store, dangling, fixed("i-" + "b".repeat(32) + "\0"));
        check(!PlaceholderFiles.isPlaceholder(dangling.toFile()), "missing backing rejected");

        Path symlinkBackingTarget = new File(store.toFile(), "i-" + "c".repeat(32)).toPath();
        Files.createSymbolicLink(symlinkBackingTarget, store.resolve(good));
        Path symlinkBacking = sticky(root.resolve("symlink-backing"), new byte[0]);
        index(store, symlinkBacking, fixed("i-" + "c".repeat(32) + "\0"));
        check(!PlaceholderFiles.isPlaceholder(symlinkBacking.toFile()), "symlink backing rejected");

        Path symlinkEntry = sticky(root.resolve("symlink-entry"), new byte[0]);
        Files.createSymbolicLink(store.resolve(key(symlinkEntry)), store.resolve(good));
        check(!PlaceholderFiles.isPlaceholder(symlinkEntry.toFile()), "symlink index rejected");

        Path directoryEntry = sticky(root.resolve("directory-entry"), new byte[0]);
        Files.createDirectory(store.resolve(key(directoryEntry)));
        check(!PlaceholderFiles.isPlaceholder(directoryEntry.toFile()), "directory index rejected");

        Path plainFile = root.resolve("plain-not-yet-sticky");
        write(plainFile, new byte[0]);
        check(!PlaceholderFiles.isPlaceholder(plainFile.toFile()), "non-sticky file rejected");
    }

    /* ---- marker/root and store-directory edges ----------------------------- */

    private static void storeAndMarkerEdges(Path base) throws Exception {
        // The proxy's read path (links_target) does not inspect the store
        // directory's mode, only store_lock does. Detection must match, or Java
        // would read the empty placeholder while the proxy reads the backing.
        Path loose = base.resolve("loose-store");
        Path store = marker(loose, 0700);
        mode(store, 0755);
        String id = "i-" + "d".repeat(32);
        write(store.resolve(id), "loose".getBytes(StandardCharsets.UTF_8));
        Path at = placeholder(store, loose.resolve("file"), id);
        check(PlaceholderFiles.isPlaceholder(at.toFile()),
                "world-readable store still resolves, matching links_target");

        // A world-writable store root is not a private store.
        Path open = base.resolve("open-root");
        Path openStore = marker(open, 0777);
        write(openStore.resolve(id), "open".getBytes(StandardCharsets.UTF_8));
        Path openFile = placeholder(openStore, open.resolve("file"), id);
        check(!PlaceholderFiles.isPlaceholder(openFile.toFile()), "group/other-writable root rejected");

        // Owner must retain rwx on the root.
        Path readOnly = base.resolve("read-only-root");
        Path readOnlyStore = marker(readOnly, 0700);
        write(readOnlyStore.resolve(id), "ro".getBytes(StandardCharsets.UTF_8));
        Path readOnlyFile = placeholder(readOnlyStore, readOnly.resolve("file"), id);
        mode(readOnly, 0500);
        check(!PlaceholderFiles.isPlaceholder(readOnlyFile.toFile()), "owner r-x root rejected");
        mode(readOnly, 0700);

        // A marker reached through a symlink does not establish a boundary.
        Path symlinked = base.resolve("symlinked-marker");
        Files.createDirectories(symlinked);
        mode(symlinked, 0700);
        Path realMarker = base.resolve("real-marker");
        write(realMarker, new byte[0]);
        mode(realMarker, 0600);
        Files.createSymbolicLink(symlinked.resolve(".v3-store-root"), realMarker);
        Path symlinkStore = symlinked.resolve(".v3-inodes");
        Files.createDirectory(symlinkStore);
        mode(symlinkStore, 0700);
        write(symlinkStore.resolve(id), "sym".getBytes(StandardCharsets.UTF_8));
        Path symlinkFile = placeholder(symlinkStore, symlinked.resolve("file"), id);
        check(!PlaceholderFiles.isPlaceholder(symlinkFile.toFile()), "symlinked marker rejected");

        // A sticky file outside any marked root is ordinary.
        Path unmarked = base.resolve("unmarked");
        Files.createDirectories(unmarked);
        Path looseFile = sticky(unmarked.resolve("file"), "unmarked".getBytes(StandardCharsets.UTF_8));
        check(!PlaceholderFiles.isPlaceholder(looseFile.toFile()), "unmarked root rejected");
    }

    /* ---- nearest store wins ------------------------------------------------ */

    private static void nestedStores(Path base) throws Exception {
        Path outer = base.resolve("outer");
        Path outerStore = marker(outer, 0700);
        String outerId = "i-" + "1".repeat(32);
        write(outerStore.resolve(outerId), "outer content\n".getBytes(StandardCharsets.UTF_8));
        Path outerFile = placeholder(outerStore, outer.resolve("outer-file"), outerId);

        Path inner = outer.resolve("inner");
        Path innerStore = marker(inner, 0700);
        String innerId = "i-" + "2".repeat(32);
        write(innerStore.resolve(innerId), "inner content\n".getBytes(StandardCharsets.UTF_8));
        Path innerFile = placeholder(innerStore, inner.resolve("deep/inner-file"), innerId);

        equals(outerStore.resolve(outerId).toFile(), PlaceholderFiles.resolve(outerFile.toFile()),
                "outer placeholder uses outer store");
        equals(innerStore.resolve(innerId).toFile(), PlaceholderFiles.resolve(innerFile.toFile()),
                "nested placeholder uses nearest store");
        check(java.util.Arrays.equals("inner content\n".getBytes(StandardCharsets.UTF_8),
                PlaceholderFiles.readAllBytes(innerFile.toFile())), "nested content");
    }

    /* ---- walk ---------------------------------------------------------------- */

    private static void walkSkipsStore(Path base) throws Exception {
        Path root = base.resolve("walker");
        Path store = marker(root, 0700);
        String id = "i-" + "3".repeat(32);
        byte[] payload = "walked payload\n".getBytes(StandardCharsets.UTF_8);
        write(store.resolve(id), payload);
        Path tree = root.resolve("tree");
        Files.createDirectory(tree);
        Path nested = placeholder(store, tree.resolve("sub/deep/file"), id);
        Path plain = tree.resolve("plain.txt");
        write(plain, "plain".getBytes(StandardCharsets.UTF_8));

        List<PlaceholderFiles.Resolved> files = PlaceholderFiles.walk(tree.toFile());
        equals(2, files.size(), "walk regular files only");
        for (PlaceholderFiles.Resolved resolved : files) {
            check(!resolved.path.toString().contains(".v3-inodes"), "walk hides the backing store");
            if (resolved.path.equals(nested)) {
                equals((long) payload.length, resolved.size, "walk reports logical size");
                equals(store.resolve(id).toFile(), resolved.file, "walk resolves backing");
            } else {
                equals(plain, resolved.path, "walk yields ordinary file");
                equals(5L, resolved.size, "walk ordinary size");
            }
        }

        try {
            PlaceholderFiles.walk(root.resolve("absent").toFile());
            check(false, "walk of a missing root must fail");
        } catch (IOException expected) { /* expected */ }

        Files.createSymbolicLink(root.resolve("linked"), root);
        try {
            PlaceholderFiles.walk(root.resolve("linked").toFile());
            check(false, "walk of a symlinked root must fail");
        } catch (IOException expected) { /* expected */ }
    }

    /* ---- helpers ----------------------------------------------------------- */

    private static byte[] little(long value) {
        byte[] out = new byte[8];
        for (int i = 0; i < 8; i++) out[i] = (byte) (value >>> (8 * i));
        return out;
    }

    private static void delete(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) delete(child);
        // Restore write permission so the store can be removed regardless of mode.
        file.setWritable(true, true);
        file.delete();
    }

    private PlaceholderFilesTest() { }
}
