package org.matonos.linuxhost.stubgen;

import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;

import com.android.apksig.ApkSigner;

import java.io.ByteArrayOutputStream;
import java.io.BufferedReader;
import java.io.File;
import java.io.RandomAccessFile;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.StringReader;
import java.nio.charset.StandardCharsets;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.security.PrivateKey;
import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Creates a signed, code-free launcher APK for one installed Flatpak ref. */
public final class StubGenerator {
    public static final int FORMAT_VERSION = 1;
    public static final String HOST_LIBRARY = "org.matonos.linuxhost";
    public static final String HOST_ACTIVITY = "org.matonos.compositor.stub.StubActivity";
    public static final String HOST_SERVICE = "org.matonos.compositor.stub.StubService";
    public static final String REF_META = "org.matonos.linuxhost.FLATPAK_REF";
    public static final String MIN_INTERFACE_META = "org.matonos.linuxhost.MIN_INTERFACE_VERSION";
    private static final String KEY_ALIAS = "matonos_flatpak_stub_v1";
    private static final int MAX_ICON_BYTES = 1024 * 1024;
    private static final long MAX_DESKTOP_BYTES = 1024 * 1024;

    public static final String GAME_CONTROLLERS = "org.matonos.permission.GAME_CONTROLLERS";
    public static final String RUN_DOWNLOADED_CODE = "org.matonos.permission.RUN_DOWNLOADED_CODE";

    /** Read only the declared Context devices and filesystem/persistent
     * patterns, never app IDs or overrides. Rule, default deny:
     *  - devices all|input  -> GAME_CONTROLLERS (unchanged);
     *  - a broad writable filesystem grant (host, home, ~, or any persistent
     *    path) means the app installs and runs code of its own, so it gets the
     *    optional per-app volume behind RUN_DOWNLOADED_CODE. Narrow ro grants
     *    (documents, specific paths) do not. */
    public static List<String> permissionsForMetadata(String metadata) throws IOException {
        boolean context = false;
        String devices = "";
        String filesystems = "";
        boolean persistent = false;
        try (BufferedReader reader = new BufferedReader(new StringReader(metadata))) {
            String line;
            while ((line = reader.readLine()) != null) {
                line = line.trim();
                if (line.startsWith("#") || line.startsWith(";")) continue;
                if (line.startsWith("[")) { context = line.equals("[Context]"); continue; }
                int equals = line.indexOf('=');
                if (context && equals >= 0) {
                    String key = line.substring(0, equals).trim();
                    if (key.equals("devices")) devices = line.substring(equals + 1).trim();
                    else if (key.equals("filesystems")) filesystems = line.substring(equals + 1).trim();
                    else if (key.equals("persistent"))
                        for (String token : line.substring(equals + 1).split(";"))
                            if (!token.trim().isEmpty()) persistent = true;
                }
            }
        }
        List<String> permissions = new ArrayList<>();
        for (String device : devices.split(";"))
            if (device.trim().equals("all") || device.trim().equals("input")) {
                permissions.add(GAME_CONTROLLERS);
                break;
            }
        if (persistent || broadFilesystem(filesystems)) permissions.add(RUN_DOWNLOADED_CODE);
        return permissions;
    }

    private static boolean broadFilesystem(String filesystems) {
        for (String item : filesystems.split(";")) {
            // Strip Flatpak access modifiers: "home:ro", "host:rw", "~/.x:create".
            String path = item.split(":", 2)[0].trim();
            if (path.isEmpty()) continue;
            if (path.equals("host") || path.equals("home") || path.equals("~") || path.startsWith("~/")) return true;
        }
        return false;
    }

    private StubGenerator() { }

    /**
     * @param workDir private scratch directory owned by the calling app
     * @param ref installed Flatpak ref, e.g. app/org.example.Editor/x86_64/stable
     * @param desktopFile Flatpak-exported .desktop file
     * @param iconPng PNG icon bytes selected by the caller
     * @param permissions Android permission names approved for this stub
     * @param packageName unique Android package name chosen by the store
     * @param output destination APK
     */
    public static File generate(File workDir, String ref, File desktopFile, byte[] iconPng,
            List<String> permissions, String packageName, File output) throws Exception {
        return generate(workDir, ref, desktopFile, iconPng, permissions, packageName, output, null, null);
    }

    public static final class RuntimeDependency {
        public final String ref, certDigest;
        public final int version;
        public RuntimeDependency(String ref, int version, String certDigest) {
            if (!validRef(ref) || !ref.startsWith("runtime/") || version <= 0 ||
                    certDigest == null || !certDigest.matches("[A-Fa-f0-9]{64}"))
                throw new IllegalArgumentException("Invalid runtime dependency");
            this.ref = ref; this.version = version; this.certDigest = certDigest;
        }
    }

    public static File generate(File workDir, String ref, File desktopFile, byte[] iconPng,
            List<String> permissions, String packageName, File output, File image,
            RuntimeDependency runtime) throws Exception {
        if (workDir == null || desktopFile == null || !desktopFile.isFile())
            throw new IllegalArgumentException("Private work directory and readable .desktop file are required");
        if (desktopFile.length() > MAX_DESKTOP_BYTES) throw new IllegalArgumentException("Desktop entry is too large");
        if (iconPng == null || iconPng.length == 0 || iconPng.length > MAX_ICON_BYTES) throw new IllegalArgumentException("PNG icon size is invalid");
        if (!validRef(ref))
            throw new IllegalArgumentException("Invalid Flatpak ref");
        if (packageName == null || packageName.length() > 255 || !packageName.matches("[A-Za-z_][A-Za-z0-9_]*(?:\\.[A-Za-z_][A-Za-z0-9_]*)+"))
            throw new IllegalArgumentException("Invalid stub package name");
        if (output == null) throw new IllegalArgumentException("Output APK is required");

        DesktopEntry entry = readDesktop(desktopFile);
        File parent = output.getAbsoluteFile().getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create output directory");
        if (!workDir.isDirectory() && !workDir.mkdirs()) throw new IOException("Cannot create private work directory");
        File unsigned = File.createTempFile("matonos-stub-", ".unsigned.apk", workDir);
        try {
            writeUnsigned(unsigned, packageName, entry, ref, iconPng, permissions, image, runtime);
            sign(unsigned, output, getOrCreateKey());
            if (image != null) checkImageEntry(output, "matonos/code.erofs");
            return output;
        } finally {
            //noinspection ResultOfMethodCallIgnored
            unsigned.delete();
        }
    }

    /** Use the same device key for the dependency digest and runtime APK. */
    public static RuntimeDependency runtimeDependency(String ref, int version) throws Exception {
        byte[] digest = java.security.MessageDigest.getInstance("SHA-256").digest(getOrCreateKey().certificate.getEncoded());
        StringBuilder hex = new StringBuilder();
        for (byte b : digest) hex.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
        return new RuntimeDependency(ref, version, hex.toString());
    }

    public static String runtimePackage(String ref) {
        if (!validRef(ref) || !ref.startsWith("runtime/")) throw new IllegalArgumentException("Invalid runtime ref");
        return "org.matonos.rt." + ref.replaceAll("[^A-Za-z0-9_]", "_");
    }

    public static File generateRuntime(File work, String ref, int version, File image, File output) throws Exception {
        KeyMaterial key = getOrCreateKey();
        byte[] digest = java.security.MessageDigest.getInstance("SHA-256").digest(key.certificate.getEncoded());
        StringBuilder hex = new StringBuilder();
        for (byte b : digest) hex.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
        BinaryManifest manifest = new BinaryManifest(runtimePackage(ref), new DesktopEntry(ref, Collections.emptyList()), ref, Collections.emptyList());
        manifest.runtime = new RuntimeDependency(ref, version, hex.toString());
        manifest.runtimeApk = true;
        return generateImageOnly(work, manifest, "matonos/runtime.erofs", image, output, key);
    }

    public static File generateExtra(File work, String pkg, File image, File output) throws Exception {
        if (pkg == null || !pkg.matches("[A-Za-z_][A-Za-z0-9_]*(?:\\.[A-Za-z_][A-Za-z0-9_]*)+"))
            throw new IllegalArgumentException("Invalid package");
        BinaryManifest manifest = new BinaryManifest(pkg, new DesktopEntry("", Collections.emptyList()), "", Collections.emptyList());
        manifest.split = "extra";
        return generateImageOnly(work, manifest, "matonos/extra.erofs", image, output, getOrCreateKey());
    }

    private static File generateImageOnly(File work, BinaryManifest manifest, String name, File image,
            File output, KeyMaterial key) throws Exception {
        if (!work.isDirectory() && !work.mkdirs()) throw new IOException("Cannot create work directory");
        File unsigned = File.createTempFile("matonos-image-", ".apk", work);
        try {
            try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(unsigned))) {
                putImage(zip, name, image);
                put(zip, "AndroidManifest.xml", manifest.encode());
            }
            sign(unsigned, output, key);
            checkImageEntry(output, name);
            return output;
        } finally { unsigned.delete(); }
    }

    // The image is the first ZIP entry: its offset is independent of every
    // manifest/resource size. Two streaming passes bound memory even for GiB images.
    private static void putImage(ZipOutputStream zip, String name, File image) throws IOException {
        if (image == null || !image.isFile()) throw new IOException("Readable image required");
        long size = image.length();
        CRC32 crc = new CRC32();
        byte[] buffer = new byte[65536];
        try (FileInputStream input = new FileInputStream(image)) {
            int n; while ((n = input.read(buffer)) != -1) crc.update(buffer, 0, n);
        }
        ZipEntry entry = new ZipEntry(name);
        entry.setMethod(ZipEntry.STORED); entry.setSize(size); entry.setCompressedSize(size); entry.setCrc(crc.getValue());
        // Explicit timestamp avoids a generated extended-time extra field.
        entry.setTime(315619200000L);
        int zip64 = size >= 0xffffffffL ? 20 : 0;
        int padding = (4096 - (30 + name.getBytes(StandardCharsets.UTF_8).length + zip64) % 4096) % 4096;
        if (padding < 4) padding += 4096;
        byte[] extra = new byte[padding];
        put16(extra, 0, 0x4d41); put16(extra, 2, padding - 4);
        entry.setExtra(extra);
        zip.putNextEntry(entry);
        try (FileInputStream input = new FileInputStream(image)) {
            int n; while ((n = input.read(buffer)) != -1) zip.write(buffer, 0, n);
        }
        zip.closeEntry();
    }

    /** Validate raw local/central records, including ZIP64 sizes and offsets.
     * Returns the image data offset; no extraction and no allocation by image size. */
    public static long checkImageEntry(File apk, String expected) throws IOException {
        if (!Arrays.asList("matonos/code.erofs", "matonos/runtime.erofs", "matonos/extra.erofs").contains(expected))
            throw new IllegalArgumentException("Unknown image entry");
        try (RandomAccessFile file = new RandomAccessFile(apk, "r")) {
            long end = -1;
            for (long pos = file.length() - 22, limit = Math.max(0, file.length() - 65557); pos >= limit; pos--) {
                file.seek(pos);
                if (u32(file) == 0x06054b50L) {
                    file.seek(pos + 20);
                    if (pos + 22 + u16(file) == file.length()) { end = pos; break; }
                }
            }
            if (end < 0) throw new IOException("Missing ZIP end record");
            file.seek(end + 4);
            if (u16(file) != 0 || u16(file) != 0) throw new IOException("Multi-disk ZIP");
            long diskCount = u16(file), count = u16(file), centralSize = u32(file), central = u32(file);
            if (diskCount != count) throw new IOException("Mismatched ZIP entry counts");
            if (count == 65535 || central == 0xffffffffL || centralSize == 0xffffffffL) {
                file.seek(end - 20);
                if (u32(file) != 0x07064b50L || u32(file) != 0) throw new IOException("Missing ZIP64 locator");
                long z64 = u64(file);
                if (u32(file) != 1) throw new IOException("Multi-disk ZIP64");
                file.seek(z64);
                if (u32(file) != 0x06064b50L || u64(file) < 44) throw new IOException("Bad ZIP64 end");
                file.skipBytes(4);
                if (u32(file) != 0 || u32(file) != 0) throw new IOException("Multi-disk ZIP64");
                diskCount = u64(file); count = u64(file); centralSize = u64(file); central = u64(file);
                if (diskCount != count) throw new IOException("ZIP64 entry count mismatch");
            }
            if (central > end || centralSize > end - central || count > centralSize / 46)
                throw new IOException("Invalid central directory bounds");
            long pos = central, result = -1; int images = 0;
            for (long i = 0; i < count; i++) {
                file.seek(pos);
                if (u32(file) != 0x02014b50L) throw new IOException("Bad central header");
                file.skipBytes(4);
                int flags = u16(file), method = u16(file); file.skipBytes(4);
                long crc = u32(file), compressed = u32(file), size = u32(file);
                int nameLen = u16(file), extraLen = u16(file), commentLen = u16(file);
                int disk = u16(file); file.skipBytes(6); long local = u32(file);
                byte[] name = new byte[nameLen], extra = new byte[extraLen]; file.readFully(name); file.readFully(extra);
                String entry = new String(name, StandardCharsets.UTF_8);
                pos += 46L + nameLen + extraLen + commentLen;
                if (pos > central + centralSize) throw new IOException("Central entry out of bounds");
                if (!entry.startsWith("matonos/") || !entry.endsWith(".erofs")) continue;
                images++;
                if (!expected.equals(entry) || disk != 0 || method != 0 || (flags & 0x49) != 0)
                    throw new IOException("Unexpected/compressed/encrypted/descriptor image");
                long[] values = zip64(extra, size, compressed, local);
                size = values[0]; compressed = values[1]; local = values[2];
                if (local > central - 30) throw new IOException("Local header out of bounds");
                file.seek(local);
                if (u32(file) != 0x04034b50L) throw new IOException("Bad local header");
                file.skipBytes(2);
                if (u16(file) != flags || u16(file) != method) throw new IOException("Local flags/method mismatch");
                file.skipBytes(4);
                if (u32(file) != crc) throw new IOException("CRC headers mismatch");
                long lc = u32(file), ls = u32(file);
                int ln = u16(file), le = u16(file);
                byte[] localName = new byte[ln], localExtra = new byte[le]; file.readFully(localName); file.readFully(localExtra);
                long[] lv = zip64(localExtra, ls, lc, 0);
                long offset = local + 30 + ln + le;
                if (!Arrays.equals(name, localName) || lv[0] != size || lv[1] != compressed || size != compressed ||
                        offset % 4096 != 0 || offset > central || size > central - offset)
                    throw new IOException("Image alignment/size/name mismatch");
                result = offset;
            }
            if (pos != central + centralSize) throw new IOException("Central directory size mismatch");
            if (images != 1 || result < 0) throw new IOException("APK must contain exactly one image");
            return result;
        }
    }

    private static long[] zip64(byte[] extra, long size, long compressed, long offset) throws IOException {
        boolean needed = size == 0xffffffffL || compressed == 0xffffffffL || offset == 0xffffffffL;
        for (int p = 0; p < extra.length;) {
            if (extra.length - p < 4) throw new IOException("Truncated ZIP extra");
            int id = (extra[p] & 255) | (extra[p+1] & 255) << 8;
            int n = (extra[p+2] & 255) | (extra[p+3] & 255) << 8; p += 4;
            if (n > extra.length - p) throw new IOException("Truncated ZIP extra value");
            if (id == 1 && needed) {
                java.nio.ByteBuffer b = java.nio.ByteBuffer.wrap(extra, p, n).order(java.nio.ByteOrder.LITTLE_ENDIAN);
                try {
                    if (size == 0xffffffffL) size = b.getLong();
                    if (compressed == 0xffffffffL) compressed = b.getLong();
                    if (offset == 0xffffffffL) offset = b.getLong();
                } catch (java.nio.BufferUnderflowException e) { throw new IOException("Truncated ZIP64 extra", e); }
                if (size < 0 || compressed < 0 || offset < 0) throw new IOException("Unsupported unsigned ZIP64 value");
                return new long[]{size, compressed, offset};
            }
            p += n;
        }
        if (needed) throw new IOException("Missing ZIP64 extra");
        return new long[]{size, compressed, offset};
    }
    private static int u16(RandomAccessFile f) throws IOException { return f.readUnsignedByte() | f.readUnsignedByte() << 8; }
    private static long u32(RandomAccessFile f) throws IOException { return (long) u16(f) | (long) u16(f) << 16; }
    private static long u64(RandomAccessFile f) throws IOException {
        long n = u32(f) | u32(f) << 32;
        if (n < 0) throw new IOException("ZIP64 value exceeds signed file offsets");
        return n;
    }

    private static DesktopEntry readDesktop(File file) throws IOException {
        Properties properties = new Properties();
        StringBuilder section = new StringBuilder();
        boolean inEntry = false;
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                String trimmed = line.trim();
                if (trimmed.startsWith("[") && trimmed.endsWith("]")) {
                    if (inEntry) break;
                    inEntry = "[Desktop Entry]".equals(trimmed);
                } else if (inEntry && !trimmed.isEmpty() && !trimmed.startsWith("#")) {
                    section.append(line).append('\n');
                }
            }
        }
        if (!inEntry) throw new IOException(".desktop file has no [Desktop Entry] group");
        properties.load(new StringReader(section.toString()));
        String name = properties.getProperty("Name", "Linux application").trim();
        if (name.isEmpty()) name = "Linux application";
        if (name.getBytes(StandardCharsets.UTF_8).length > 4096) name = "Linux application";
        List<String> mimes = new ArrayList<>();
        for (String item : properties.getProperty("MimeType", "").split(";")) {
            String mime = item.trim();
            if (mime.length() <= 255 && mime.matches("[A-Za-z0-9!#$&^_.+-]+/[A-Za-z0-9!#$&^_.+-]+") && !mimes.contains(mime)) mimes.add(mime);
        }
        if (mimes.size() > 128) mimes.subList(128, mimes.size()).clear();
        return new DesktopEntry(name, mimes);
    }

    private static boolean validRef(String ref) {
        if (ref == null || ref.length() < 12 || ref.length() > 512) return false;
        String[] parts = ref.split("/", -1);
        if (parts.length != 4 || !(parts[0].equals("app") || parts[0].equals("runtime")) ||
                !validAppId(parts[1]) || !validRefPart(parts[2]) || !validRefPart(parts[3])) return false;
        for (String part : parts) if (part.equals(".") || part.equals("..")) return false;
        return true;
    }

    private static boolean validRefPart(String value) {
        return value.length() > 0 && value.length() <= 96 && value.matches("[A-Za-z0-9_.-]+") &&
                !value.equals(".") && !value.equals("..");
    }

    private static boolean validAppId(String value) {
        if (value.length() < 3 || value.length() > 255 || value.startsWith(".") || value.endsWith(".")) return false;
        int lastDot = value.lastIndexOf('.');
        if (lastDot < 0) return false;
        int component = 0;
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if (c == '.') {
                if (component == 0 || component > 63 || value.charAt(i - 1) == '-') return false;
                component = 0;
            } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_') {
                if (component == 0 && !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
                component++;
            } else if (c == '-' && i > lastDot && component > 0 && i + 1 < value.length() && value.charAt(i + 1) != '.') {
                component++;
            } else return false;
        }
        return component > 0 && component <= 63;
    }

    private static void writeUnsigned(File file, String pkg, DesktopEntry entry, String ref,
            byte[] icon, List<String> permissions, File image, RuntimeDependency runtime) throws IOException {
        List<String> cleanPermissions = new ArrayList<>();
        if (permissions != null) for (String p : permissions) {
            if (cleanPermissions.size() >= 128 || (p != null && p.length() > 255)) throw new IllegalArgumentException("Too many or oversized Android permissions");
            if (p == null || !p.matches("[A-Za-z0-9_.]+")) throw new IllegalArgumentException("Invalid Android permission: " + p);
            if (!cleanPermissions.contains(p)) cleanPermissions.add(p);
        }
        BinaryManifest encoder = new BinaryManifest(pkg, entry, ref, cleanPermissions);
        encoder.runtime = runtime;
        byte[] manifest = encoder.encode();
        try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(file))) {
            if (image != null) putImage(zip, "matonos/code.erofs", image);
            put(zip, "AndroidManifest.xml", manifest);
            // Supply a real drawable resource so Android launchers can resolve the app icon.
            put(zip, "resources.arsc", iconResourceTable(pkg));
            put(zip, "classes.dex", emptyDex());
            put(zip, "res/drawable/foreground.png", icon);
            put(zip, "res/drawable/icon.xml", new BinaryManifest("", entry, "", Collections.emptyList()).adaptiveIcon());
        }
    }

    /** Adaptive icon XML, foreground bitmap and background color resources.
     * The background must be a reference: density-specific drawable inflation
     * resolves a layer's resource ID, which is zero for an inline XML color.
     */
    private static byte[] iconResourceTable(String pkg) throws IOException {
        byte[] global = resourceStringPool("res/drawable/icon.xml", "res/drawable/foreground.png");
        byte[] types = resourceStringPool("drawable");
        byte[] keys = resourceStringPool("icon", "foreground", "background");
        byte[] spec = new byte[28];
        put16(spec,0,0x0202); put16(spec,2,16); putInt(spec,4,spec.length);
        spec[8]=1; putInt(spec,12,3);
        byte[] type = new byte[144];
        put16(type,0,0x0201); put16(type,2,84); putInt(type,4,type.length);
        type[8]=1; putInt(type,12,3); putInt(type,16,96); putInt(type,20,64);
        putInt(type,84,0); putInt(type,88,16); putInt(type,92,32);
        for (int i=0;i<3;i++) {
            int offset=96+i*16; put16(type,offset,8); putInt(type,offset+4,i);
            put16(type,offset+8,8); type[offset+11]=(byte)(i==2?0x1c:3); putInt(type,offset+12,i==2?0xfff5f5f5:i);
        }
        byte[] header = new byte[288];
        put16(header,0,0x0200); put16(header,2,288);
        putInt(header,4,288+types.length+keys.length+spec.length+type.length); putInt(header,8,0x7f);
        byte[] name = pkg.getBytes(StandardCharsets.UTF_16LE);
        System.arraycopy(name,0,header,12,Math.min(name.length,254));
        putInt(header,268,288); putInt(header,272,1);
        putInt(header,276,288+types.length); putInt(header,280,3);
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        BinaryManifest.write16(out,2); BinaryManifest.write16(out,12);
        BinaryManifest.write32(out,12+global.length+header.length+types.length+keys.length+spec.length+type.length);
        BinaryManifest.write32(out,1);
        out.write(global); out.write(header); out.write(types); out.write(keys); out.write(spec); out.write(type);
        return out.toByteArray();
    }

    private static byte[] resourceStringPool(String... values) throws IOException {
        BinaryManifest encoder = new BinaryManifest("",new DesktopEntry("",Collections.emptyList()),"",Collections.emptyList());
        for (String value: values) encoder.str(value);
        ByteArrayOutputStream out = new ByteArrayOutputStream(); encoder.writeStringPool(out); return out.toByteArray();
    }

    private static byte[] emptyDex() {
        // Minimal DEX header plus the required map list (header_item, map_list).
        byte[] dex = new byte[140];
        byte[] magic = "dex\n035\0".getBytes(StandardCharsets.US_ASCII);
        System.arraycopy(magic, 0, dex, 0, magic.length);
        putInt(dex, 32, dex.length); putInt(dex, 36, 112); putInt(dex, 40, 0x12345678);
        putInt(dex, 52, 112); putInt(dex, 104, 28); putInt(dex, 108, 112);
        putInt(dex, 112, 2); // map_list.size
        put16(dex, 116, 0x0000); put32(dex, 120, 1); put32(dex, 124, 0);
        put16(dex, 128, 0x1000); put32(dex, 132, 1); put32(dex, 136, 112);
        try {
            byte[] sha1 = java.security.MessageDigest.getInstance("SHA-1").digest(Arrays.copyOfRange(dex, 32, dex.length));
            System.arraycopy(sha1, 0, dex, 12, sha1.length);
            java.util.zip.Adler32 adler = new java.util.zip.Adler32();
            adler.update(dex, 12, dex.length - 12); putInt(dex, 8, (int) adler.getValue());
        } catch (Exception impossible) { throw new AssertionError(impossible); }
        return dex;
    }

    private static void putInt(byte[] out, int offset, int value) {
        out[offset] = (byte) value; out[offset + 1] = (byte) (value >>> 8);
        out[offset + 2] = (byte) (value >>> 16); out[offset + 3] = (byte) (value >>> 24);
    }
    private static void put16(byte[] out, int offset, int value) {
        out[offset] = (byte) value; out[offset + 1] = (byte) (value >>> 8);
    }
    private static void put32(byte[] out, int offset, int value) { putInt(out, offset, value); }

    private static void put(ZipOutputStream zip, String name, byte[] bytes) throws IOException {
        ZipEntry entry = new ZipEntry(name);
        // Android's package parser reads binary XML directly from the APK; keep it uncompressed.
        // Store all v1 contents for a simpler, deterministic ZIP layout.
        CRC32 crc = new CRC32(); crc.update(bytes);
        entry.setMethod(ZipEntry.STORED); entry.setSize(bytes.length);
        entry.setCompressedSize(bytes.length); entry.setCrc(crc.getValue());
        zip.putNextEntry(entry); zip.write(bytes); zip.closeEntry();
    }

    /** Read-only attestation: do not mint a replacement key during verification. */
    public static byte[] getExistingSigningCertificate() throws Exception {
        KeyStore store=KeyStore.getInstance("AndroidKeyStore");store.load(null);
        java.security.cert.Certificate certificate=store.getCertificate(KEY_ALIAS);
        return certificate!=null?certificate.getEncoded():null;
    }

    private static synchronized KeyMaterial getOrCreateKey() throws Exception {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore"); store.load(null);
        if (!store.containsAlias(KEY_ALIAS)) {
            KeyPairGenerator generator = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_EC, "AndroidKeyStore");
            generator.initialize(new KeyGenParameterSpec.Builder(KEY_ALIAS,
                    KeyProperties.PURPOSE_SIGN | KeyProperties.PURPOSE_VERIFY)
                    .setAlgorithmParameterSpec(new java.security.spec.ECGenParameterSpec("secp256r1"))
                    .setDigests(KeyProperties.DIGEST_SHA256).build());
            generator.generateKeyPair();
            store.load(null);
        }
        PrivateKey key = (PrivateKey) store.getKey(KEY_ALIAS, null);
        X509Certificate certificate = (X509Certificate) store.getCertificate(KEY_ALIAS);
        if (key == null || certificate == null) throw new IllegalStateException("Stub signing key unavailable in Android Keystore");
        return new KeyMaterial(key, certificate);
    }

    private static void sign(File input, File output, KeyMaterial material) throws Exception {
        ApkSigner.SignerConfig signer = new ApkSigner.SignerConfig.Builder("matonos-stub", material.key,
                Collections.singletonList(material.certificate)).build();
        new ApkSigner.Builder(Collections.singletonList(signer)).setInputApk(input).setOutputApk(output)
                // Stubs require API 30; v2/v3 cover their supported Android versions.
                .setV1SigningEnabled(false).setV2SigningEnabled(true).setV3SigningEnabled(true)
                .setV4SigningEnabled(false).setAlignmentPreserved(true).build().sign();
    }

    private static final class KeyMaterial {
        final PrivateKey key; final X509Certificate certificate;
        KeyMaterial(PrivateKey key, X509Certificate certificate) { this.key = key; this.certificate = certificate; }
    }
    private static final class DesktopEntry {
        final String name; final List<String> mimeTypes;
        DesktopEntry(String name, List<String> mimeTypes) { this.name = name; this.mimeTypes = mimeTypes; }
    }

    /** Minimal Android binary XML encoder for the manifest vocabulary used by stubs. */
    private static final class BinaryManifest {
        private static final String ANDROID = "http://schemas.android.com/apk/res/android";
        private static final int ANDROID_URI = 0x01000000;
        private final String pkg, label, ref;
        private final List<String> mimes, permissions;
        private final LinkedHashMap<String, Integer> strings = new LinkedHashMap<>();
        private final List<Chunk> nodes = new ArrayList<>();
        BinaryManifest(String pkg, DesktopEntry entry, String ref, List<String> permissions) {
            this.pkg = pkg; this.label = entry.name; this.ref = ref; this.mimes = entry.mimeTypes; this.permissions = permissions;
        }
        RuntimeDependency runtime;
        String split;
        boolean runtimeApk;
        byte[] encode() throws IOException {
            for (String s : Arrays.asList(ANDROID, "android", "manifest", "package", "versionCode", "versionName", "uses-sdk", "minSdkVersion", "targetSdkVersion", "uses-permission", "name", "uses-library", "required", "application", "label", "hasCode", "activity", "exported", "meta-data", "value", "intent-filter", "action", "category", "data", "mimeType", "service", "org.matonos.compositor.stub.StubActivity", "org.matonos.compositor.stub.StubService", "android.intent.action.MAIN", "android.intent.category.LAUNCHER", "android.intent.action.VIEW", "android.intent.category.DEFAULT", "android.intent.category.BROWSABLE", HOST_LIBRARY, REF_META, MIN_INTERFACE_META, pkg, label, ref, "1", "1.0", "30", "36")) str(s);
            for (String p : permissions) str(p);
            for (String mime : mimes) str(mime);
            namespace(true);
            List<Attr> root = new ArrayList<>(attrs(a("package", pkg), ai("versionCode", runtimeApk ? runtime.version : 10), a("versionName", "1.0")));
            if (split != null) root.add(a("split", split));
            start("manifest", null, root);
            start("uses-sdk", null, attrs(ai("minSdkVersion", 30), ai("targetSdkVersion", 36)));
            end("uses-sdk");
            if (runtimeApk || split != null) {
                start("application", null, attrs(ab("hasCode", false)));
                if (runtimeApk) startEnd("static-library", attrs(a("name", runtime.ref), ai("version", runtime.version)));
                end("application"); end("manifest"); namespace(false);
                return finish();
            }
            startEnd("uses-permission",attrs(a("name","android.permission.FOREGROUND_SERVICE")));
            start("queries",null,attrs());
            startEnd("package",attrs(a("name","org.matonos.compositor")));
            end("queries");
            for (String p : permissions) startEnd("uses-permission", attrs(a("name", p)));
            start("application", null, attrs(a("label", label), new Attr("icon", "@drawable/icon", 0x7f010000, 1), ab("hasCode", true),new Attr("theme","@android:style/Theme.Material.Light.NoActionBar",android.R.style.Theme_Material_Light_NoActionBar,1)));
            if (runtime != null) startEnd("uses-static-library", attrs(a("name", runtime.ref), ai("version", runtime.version), a("certDigest", runtime.certDigest)));
            startEnd("uses-library", attrs(a("name", HOST_LIBRARY), ab("required", true)));
            start("activity", null, attrs(a("name", HOST_ACTIVITY), ab("exported", true), a("label", label), new Attr("icon", "@drawable/icon", 0x7f010000, 1)));
            startEnd("meta-data", attrs(a("name", REF_META), a("value", ref)));
            startEnd("meta-data", attrs(a("name", MIN_INTERFACE_META), ai("value", 2)));
            start("intent-filter", null, attrs());
            startEnd("action", attrs(a("name", "android.intent.action.MAIN")));
            startEnd("category", attrs(a("name", "android.intent.category.LAUNCHER")));
            end("intent-filter");
            for (String mime : mimes) {
                start("intent-filter", null, attrs());
                startEnd("action", attrs(a("name", "android.intent.action.VIEW")));
                startEnd("category", attrs(a("name", "android.intent.category.DEFAULT")));
                startEnd("category", attrs(a("name", "android.intent.category.BROWSABLE")));
                startEnd("data", attrs(a("mimeType", mime)));
                end("intent-filter");
            }
            end("activity");
            startEnd("service", attrs(a("name", HOST_SERVICE), ab("exported", false)));
            end("application"); end("manifest"); namespace(false);
            return finish();
        }
        private byte[] finish() throws IOException {
            ByteArrayOutputStream body = new ByteArrayOutputStream();
            writeStringPool(body); writeResourceMap(body);
            for (Chunk chunk : nodes) chunk.write(body);
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            write16(out, 0x0003); write16(out, 8); write32(out, 8 + body.size()); body.writeTo(out); return out.toByteArray();
        }
        byte[] adaptiveIcon() throws IOException {
            namespace(true);
            start("adaptive-icon", null, attrs());
            startEnd("background", attrs(new Attr("drawable", "@drawable/background", 0x7f010002, 1)));
            startEnd("foreground", attrs(new Attr("drawable", "@drawable/foreground", 0x7f010001, 1)));
            end("adaptive-icon"); namespace(false);
            ByteArrayOutputStream body=new ByteArrayOutputStream();
            writeStringPool(body);writeResourceMap(body);
            for (Chunk chunk:nodes) chunk.write(body);
            ByteArrayOutputStream out=new ByteArrayOutputStream();
            write16(out,3);write16(out,8);write32(out,8+body.size());body.writeTo(out);
            return out.toByteArray();
        }
        private int str(String s) { Integer i = strings.get(s); if (i != null) return i; int n = strings.size(); strings.put(s, n); return n; }
        private Attr a(String name, String value) { return new Attr(name, value, 0, 3); }
        private Attr ai(String name, int value) { return new Attr(name, Integer.toString(value), value, 0x10); }
        private Attr ab(String name, boolean value) { return new Attr(name, value ? "true" : "false", value ? 1 : 0, 0x12); }
        private List<Attr> attrs(Attr... a) { return Arrays.asList(a); }
        private void namespace(boolean start) throws IOException { int t = start ? 0x0100 : 0x0101; nodes.add(Chunk.namespace(t, str("android"), str(ANDROID))); }
        private void start(String name, String text, List<Attr> attrs) throws IOException {
            // Android's TypedArray retrieval walks attributes by resource ID.
            // Unsorted attributes can silently disappear during manifest parsing.
            attrs = new ArrayList<>(attrs);
            attrs.sort((left, right) -> Integer.compareUnsigned(resourceId(left.name), resourceId(right.name)));
            for (Attr attr : attrs) { str(attr.name); str(attr.value); }
            nodes.add(Chunk.start(str(name), attrs, strings, str(ANDROID)));
        }
        private void startEnd(String name, List<Attr> attrs) throws IOException { start(name, null, attrs); end(name); }
        private void end(String name) throws IOException { nodes.add(Chunk.end(str(name))); }
        private void writeStringPool(ByteArrayOutputStream out) throws IOException {
            ByteArrayOutputStream data = new ByteArrayOutputStream(); List<Integer> offsets = new ArrayList<>();
            for (String s : strings.keySet()) {
                offsets.add(data.size()); byte[] utf8 = s.getBytes(StandardCharsets.UTF_8); write8Length(data, s.length()); write8Length(data, utf8.length); data.write(utf8); data.write(0);
            }
            while ((data.size() & 3) != 0) data.write(0);
            int header = 28, start = header + offsets.size() * 4, size = start + data.size();
            write16(out, 1); write16(out, header); write32(out, size); write32(out, offsets.size()); write32(out, 0); write32(out, 0x100); write32(out, start); write32(out, 0);
            for (int offset : offsets) write32(out, offset); data.writeTo(out);
        }
        private void writeResourceMap(ByteArrayOutputStream out) throws IOException {
            int[] ids = new int[strings.size()];
            for (String name : Arrays.asList("version", "certDigest", "theme", "drawable", "name", "label", "icon", "exported", "hasCode", "required", "value", "mimeType", "versionCode", "versionName", "minSdkVersion", "targetSdkVersion")) {
                Integer i = strings.get(name); if (i != null) ids[i] = resourceId(name);
            }
            write16(out, 0x0180); write16(out, 8); write32(out, 8 + ids.length * 4); for (int id : ids) write32(out, id);
        }
        private int resourceId(String name) {
            switch (name) {
                case "version": return 0x01010519;
                case "certDigest": return 0x01010548;
                case "theme": return 0x01010000;
                case "drawable": return 0x01010199; case "icon": return 0x01010002; case "name": return 0x01010003; case "label": return 0x01010001; case "exported": return 0x01010010;
                case "hasCode": return 0x0101000c; case "required": return 0x0101028e; case "value": return 0x01010024;
                case "mimeType": return 0x01010026; case "versionCode": return 0x0101021b; case "versionName": return 0x0101021c;
                case "minSdkVersion": return 0x0101020c; case "targetSdkVersion": return 0x01010270; default: return 0;
            }
        }
        private static void write8Length(ByteArrayOutputStream out, int n) {
            if (n < 0 || n > 32767) throw new IllegalArgumentException("Manifest string is too large");
            if (n > 127) { out.write((n >> 8) | 0x80); out.write(n & 0xff); } else out.write(n);
        }
        private static void write16(ByteArrayOutputStream out, int n) { out.write(n & 255); out.write((n >> 8) & 255); }
        private static void write32(ByteArrayOutputStream out, int n) { write16(out, n); write16(out, n >>> 16); }
        private static final class Attr {
            final String name, value; final int data, type;
            Attr(String name, String value, int data, int type) { this.name = name; this.value = value; this.data = data; this.type = type; }
        }
        private static final class Chunk {
            final byte[] bytes; Chunk(byte[] bytes) { this.bytes = bytes; }
            void write(ByteArrayOutputStream out) throws IOException { out.write(bytes); }
            static Chunk namespace(int type, int prefix, int uri) throws IOException {
                ByteArrayOutputStream out = node(type, 24); write32(out, prefix); write32(out, uri); return new Chunk(out.toByteArray());
            }
            static Chunk start(int name, List<Attr> attrs, Map<String, Integer> strings, int androidUri) throws IOException {
                ByteArrayOutputStream out = node(0x0102, 36 + attrs.size() * 20);
                write32(out, -1); write32(out, name);
                write16(out, 20); write16(out, 20); write16(out, attrs.size()); write16(out, 0); write16(out, 0); write16(out, 0);
                for (Attr a : attrs) {
                    Integer nameIndex = strings.get(a.name);
                    Integer valueIndex = strings.get(a.value);
                    if (nameIndex == null || valueIndex == null) throw new IllegalStateException("Uninterned manifest attribute " + a.name + "=" + a.value);
                    write32(out, ("package".equals(a.name) || "split".equals(a.name)) ? -1 : androidUri); write32(out, nameIndex); write32(out, valueIndex);
                    write16(out, 8); out.write(0); out.write(a.type); write32(out, a.type == 3 ? strings.get(a.value) : a.data);
                }
                return new Chunk(out.toByteArray());
            }
            static Chunk end(int name) throws IOException { ByteArrayOutputStream out = node(0x0103, 24); write32(out, -1); write32(out, name); return new Chunk(out.toByteArray()); }
            private static ByteArrayOutputStream node(int type, int size) throws IOException { ByteArrayOutputStream out = new ByteArrayOutputStream(); write16(out, type); write16(out, 16); write32(out, size); write32(out, 1); write32(out, -1); return out; }
        }
    }
}
