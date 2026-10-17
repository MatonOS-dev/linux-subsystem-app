package org.matonos.linuxhost.stubgen;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.OpenOption;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Pattern;

/**
 * Resolves compatibility-proxy <em>indexed regular placeholders</em> to the
 * backing object that actually carries the content.
 *
 * <p>The v3.5 compatibility proxy emulates hardlinks only inside a marked store:
 * the first {@code link}/{@code linkat} moves the content to
 * {@code .v3-inodes/i-<32 hex>} and replaces every linked name with an empty
 * regular file carrying {@code S_ISVTX} plus a constant-time index entry
 * {@code p-<16 hex st_dev>-<16 hex st_ino>} whose NUL-terminated content names
 * the relative backing id. Programs under the proxy are redirected to the
 * backing file; Java code reads staging trees natively and would otherwise see
 * empty files. This class performs exactly the proxy's recognition
 * ({@code linux/compat-proxy/links.c}, {@code links_target()}) without the
 * proxy:
 *
 * <ul>
 *   <li>the final component must be a regular file (no symlink) with
 *       {@code S_ISVTX};</li>
 *   <li>the nearest ancestor of the parent that carries an owner-owned regular
 *       {@code .v3-store-root} marker <em>and</em> is an owner-only, same-device
 *       private directory establishes the store;</li>
 *   <li>the store's {@code .v3-inodes/} holds a {@code p-<dev>-<ino>} entry that
 *       is a regular file whose first 35 bytes are a NUL-terminated
 *       {@code i-<32 hex>} id; any other value (including {@code ..}, absolute
 *       paths or separators) is rejected. The directory's own mode is not
 *       inspected, matching the proxy's read path ({@code links_target}), which
 *       only enforces owner-only access when it writes ({@code store_lock});</li>
 *   <li>the named backing file must be a regular file (no symlink) inside the
 *       same store.</li>
 * </ul>
 *
 * <p>A sticky file without a valid index is an ordinary file. Detection is
 * read-only: this class never creates, writes or modifies a placeholder, store,
 * index entry or backing object. On device the filesystem metadata comes from
 * {@code android.system.Os.lstat}; on a host JVM (tests) the {@code unix}
 * attribute view is used instead. Both are reached through a tiny pluggable
 * backend, so no Android type is linked when the class is used on a host.
 */
public final class PlaceholderFiles {
    private static final String MARKER = ".v3-store-root";
    private static final String STORE = ".v3-inodes";
    /** Backing id length including the terminating NUL read from the index. */
    private static final int ID_LENGTH = 35;
    private static final Pattern BACKING = Pattern.compile("i-[0-9a-f]{32}");

    private static final int S_IFMT = 0170000;
    private static final int S_IFREG = 0100000;
    private static final int S_IFDIR = 0040000;
    private static final int S_ISVTX = 01000;
    /** Owner rwx required by the proxy's {@code private_dir()}. */
    private static final int OWNER_RWX = 0300;
    /** No group/other write permitted by the proxy's {@code private_dir()}. */
    private static final int GROUP_OTHER_WRITE = 0022;

    private PlaceholderFiles() { }

    /** Filesystem metadata needed to recognize a placeholder. */
    public static final class Stat {
        public final long dev;
        public final long ino;
        public final int mode;
        public final int uid;
        Stat(long dev, long ino, int mode, int uid) {
            this.dev = dev; this.ino = ino; this.mode = mode; this.uid = uid;
        }
        public boolean regular() { return (mode & S_IFMT) == S_IFREG; }
        public boolean directory() { return (mode & S_IFMT) == S_IFDIR; }
        public boolean sticky() { return (mode & S_ISVTX) != 0; }
    }

    /** A resolved regular file plus its logical size. */
    public static final class Resolved {
        /** The logical path (where the caller looked). */
        public final Path path;
        /** The backing file on disk (equal to {@code path} for ordinary files). */
        public final File file;
        /** Logical content length, from the backing object for a placeholder. */
        public final long size;
        Resolved(Path path, File file, long size) {
            this.path = path; this.file = file; this.size = size;
        }
    }

    /** Receives every regular file under a walked tree. */
    public interface Visitor {
        void visit(Resolved resolved) throws IOException;
    }

    /** Filesystem metadata source, overridable by tests. */
    public interface Backend {
        /** Metadata without following the final symlink, or null when absent. */
        Stat lstat(String path) throws IOException;
        /** Effective uid that owns private stores. */
        int uid();
    }

    private static final Backend DEVICE = new Backend() {
        @Override public Stat lstat(String path) throws IOException {
            try {
                return Android.lstat(path);
            } catch (Throwable unavailable) {
                // android.jar absent, or its stubbed method ran on a host JVM.
            }
            try {
                Map<String, Object> attributes = Files.readAttributes(Paths.get(path),
                        "unix:dev,ino,mode,uid", LinkOption.NOFOLLOW_LINKS);
                return new Stat(((Number) attributes.get("dev")).longValue(),
                        ((Number) attributes.get("ino")).longValue(),
                        ((Number) attributes.get("mode")).intValue(),
                        ((Number) attributes.get("uid")).intValue());
            } catch (Exception unsupported) {
                return null;
            }
        }
        @Override public int uid() {
            try {
                return Android.euid();
            } catch (Throwable unavailable) { /* host JVM */ }
            try {
                return ((Number) Files.getAttribute(Paths.get("/proc/self"), "unix:uid")).intValue();
            } catch (Exception unavailable) { /* fall through */ }
            try {
                return ((Number) Files.getAttribute(Paths.get(System.getProperty("user.dir")),
                        "unix:uid")).intValue();
            } catch (Exception unavailable) {
                return -1;
            }
        }
    };

    private static volatile Backend backend = DEVICE;

    /** Installs a metadata source; null restores the device/host default. */
    public static void install(Backend value) { backend = value == null ? DEVICE : value; }

    private static Backend backend() { return backend; }

    /**
     * The backing file for {@code file}, or {@code file} itself when it is not a
     * placeholder. The returned path is readable as the logical content.
     */
    public static File resolve(File file) throws IOException {
        File backing = detect(file);
        return backing == null ? file : backing;
    }

    /** {@link #resolve(File)} for a {@link Path}. */
    public static File resolve(Path path) throws IOException {
        return resolve(path.toFile());
    }

    /** True only for a regular S_ISVTX file with a valid store index entry. */
    public static boolean isPlaceholder(File file) throws IOException {
        return detect(file) != null;
    }

    /** Opens the logical content (backing object for a placeholder). */
    public static InputStream open(File file) throws IOException {
        return new BufferedInputStream(Files.newInputStream(resolve(file).toPath()));
    }

    /** Reads the complete logical content. */
    public static byte[] readAllBytes(File file) throws IOException {
        return Files.readAllBytes(resolve(file).toPath());
    }

    /** Logical content length. */
    public static long size(File file) throws IOException {
        return Files.size(resolve(file).toPath());
    }

    /** Copies the logical content into {@code out}; returns the byte count. */
    public static long copy(File file, OutputStream out) throws IOException {
        long total = 0;
        byte[] buffer = new byte[65536];
        try (InputStream in = open(file)) {
            int n;
            while ((n = in.read(buffer)) != -1) { out.write(buffer, 0, n); total += n; }
        }
        return total;
    }

    /** Opens a read channel on the logical content. */
    public static FileChannel channel(File file, OpenOption... options) throws IOException {
        OpenOption[] effective = options.length == 0
                ? new OpenOption[] {StandardOpenOption.READ} : options;
        return FileChannel.open(resolve(file).toPath(), effective);
    }

    /**
     * Logical link count from the persistent {@code i-<id>.count} sidecar, or the
     * backing file's physical count when the sidecar is absent. Ordinary files
     * report their own count.
     */
    public static long linkCount(File file) throws IOException {
        Stat self = backend().lstat(file.getPath());
        Store store = self == null ? null : findStore(file);
        String id = store == null ? null : store.backingId(self);
        if (id != null) {
            File count = new File(store.store, id + ".count");
            try {
                byte[] raw = Files.readAllBytes(count.toPath());
                if (raw.length == 8) {
                    long value = 0;
                    for (int i = 7; i >= 0; i--) value = (value << 8) | (raw[i] & 0xffL);
                    return value;
                }
            } catch (IOException absent) { /* fall through to physical count */ }
        }
        Object value = Files.getAttribute(resolve(file).toPath(), "unix:nlink");
        return value instanceof Number ? ((Number) value).longValue() : 1L;
    }

    /**
     * Walks {@code root} without following symlinks and yields every regular
     * file, resolving placeholders and reporting the logical size. The hidden
     * {@code .v3-inodes} store directory is skipped, mirroring the proxy, so the
     * physical backing store is not exposed as payload.
     */
    public static void walk(File root, Visitor visitor) throws IOException {
        if (!root.isDirectory() || Files.isSymbolicLink(root.toPath()))
            throw new IOException("Missing tree root");
        walkInto(root, visitor);
    }

    /** All regular files under {@code root}, sorted by logical path. */
    public static List<Resolved> walk(File root) throws IOException {
        List<Resolved> files = new ArrayList<>();
        walk(root, files::add);
        files.sort(Comparator.comparing(r -> r.path.toString()));
        return files;
    }

    private static void walkInto(File directory, Visitor visitor) throws IOException {
        File[] children = directory.listFiles();
        if (children == null) throw new IOException("Cannot list " + directory);
        for (File child : children) {
            BasicFileAttributes attributes = Files.readAttributes(child.toPath(),
                    BasicFileAttributes.class, LinkOption.NOFOLLOW_LINKS);
            if (attributes.isDirectory()) {
                if (child.getName().equals(STORE)) continue;
                walkInto(child, visitor);
            } else if (attributes.isRegularFile()) {
                visitor.visit(new Resolved(child.toPath(), resolve(child), size(child)));
            }
        }
    }

    /* ------------------------------- detection ------------------------------ */

    /** The backing file, or null when {@code file} is not an index placeholder. */
    private static File detect(File file) throws IOException {
        Stat self = backend().lstat(file.getPath());
        if (self == null || !self.regular() || !self.sticky()) return null;
        Store store = findStore(file);
        if (store == null) return null;
        String id = store.backingId(self);
        return id == null ? null : new File(store.store, id);
    }

    private static final class Store {
        final File store;
        Store(File store) { this.store = store; }

        /** Validates the index entry for {@code self}; null when it is not valid. */
        String backingId(Stat self) throws IOException {
            File entry = new File(store, String.format(Locale.ROOT,
                    "p-%016x-%016x", self.dev, self.ino));
            Stat entryStat = backend().lstat(entry.getPath());
            if (entryStat == null || !entryStat.regular()) return null;
            byte[] raw;
            try (InputStream in = Files.newInputStream(entry.toPath(), LinkOption.NOFOLLOW_LINKS)) {
                raw = in.readNBytes(ID_LENGTH);
            }
            if (raw.length != ID_LENGTH || raw[ID_LENGTH - 1] != 0) return null;
            String value = new String(raw, 0, ID_LENGTH - 1, StandardCharsets.ISO_8859_1);
            if (!BACKING.matcher(value).matches()) return null;
            Stat backingStat = backend().lstat(new File(store, value).getPath());
            if (backingStat == null || !backingStat.regular()) return null;
            return value;
        }
    }

    /**
     * Nearest ancestor of {@code file}'s parent that is a valid marked store
     * root. The store's {@code .v3-inodes} directory is required only
     * implicitly, exactly as the proxy's read path does: the index entry must
     * exist and name a regular backing file.
     */
    private static Store findStore(File file) throws IOException {
        File parent = file.getParentFile();
        if (parent == null) return null;
        Stat parentStat = backend().lstat(parent.getPath());
        if (parentStat == null) return null;
        long dev = parentStat.dev;
        File root = parent;
        for (;;) {
            if (marked(root, dev)) return new Store(new File(root, STORE));
            File up = root.getParentFile();
            if (up == null || up.equals(root)) return null;
            root = up;
        }
    }

    /** A marked directory that is an owner-only, same-device private directory. */
    private static boolean marked(File root, long dev) throws IOException {
        Backend backend = backend();
        Stat marker = backend.lstat(new File(root, MARKER).getPath());
        if (marker == null || !marker.regular() || marker.uid != backend.uid()) return false;
        Stat dir = backend.lstat(root.getPath());
        return dir != null && dir.directory() && dir.dev == dev && dir.uid == backend.uid()
                && (dir.mode & OWNER_RWX) == OWNER_RWX && (dir.mode & GROUP_OTHER_WRITE) == 0;
    }

    /**
     * Device metadata via {@code android.system.Os}, reached reflectively so a
     * host JVM without android.jar simply falls back instead of failing linkage.
     */
    private static final class Android {
        private static Method lstat;
        private static Field dev, ino, mode, uid;
        private static Method geteuid;

        static synchronized Stat lstat(String path) throws Exception {
            if (lstat == null) {
                Class<?> os = Class.forName("android.system.Os");
                Class<?> stat = Class.forName("android.system.StructStat");
                lstat = os.getMethod("lstat", String.class);
                dev = stat.getField("st_dev");
                ino = stat.getField("st_ino");
                mode = stat.getField("st_mode");
                uid = stat.getField("st_uid");
                geteuid = os.getMethod("geteuid");
            }
            Object value = lstat.invoke(null, path);
            return new Stat(((Number) dev.get(value)).longValue(),
                    ((Number) ino.get(value)).longValue(),
                    ((Number) mode.get(value)).intValue(),
                    ((Number) uid.get(value)).intValue());
        }

        static synchronized int euid() throws Exception {
            if (geteuid == null) {
                Class<?> os = Class.forName("android.system.Os");
                geteuid = os.getMethod("geteuid");
            }
            return ((Number) geteuid.invoke(null)).intValue();
        }
    }
}
