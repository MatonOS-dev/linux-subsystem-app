package org.matonos.linuxhost.stubgen;

import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;

import com.android.apksig.ApkSigner;

import java.io.ByteArrayOutputStream;
import java.io.BufferedReader;
import java.io.File;
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
        if (workDir == null || desktopFile == null || !desktopFile.isFile())
            throw new IllegalArgumentException("Private work directory and readable .desktop file are required");
        if (iconPng == null || iconPng.length == 0) throw new IllegalArgumentException("PNG icon is required");
        if (ref == null || !ref.matches("(?:app|runtime)/[A-Za-z0-9._-]+/[A-Za-z0-9_-]+/[A-Za-z0-9._-]+"))
            throw new IllegalArgumentException("Invalid Flatpak ref");
        if (packageName == null || !packageName.matches("[A-Za-z_][A-Za-z0-9_]*(?:\\.[A-Za-z_][A-Za-z0-9_]*)+"))
            throw new IllegalArgumentException("Invalid stub package name");
        if (output == null) throw new IllegalArgumentException("Output APK is required");

        DesktopEntry entry = readDesktop(desktopFile);
        File parent = output.getAbsoluteFile().getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create output directory");
        if (!workDir.isDirectory() && !workDir.mkdirs()) throw new IOException("Cannot create private work directory");
        File unsigned = File.createTempFile("matonos-stub-", ".unsigned.apk", workDir);
        try {
            writeUnsigned(unsigned, packageName, entry, ref, iconPng, permissions);
            sign(unsigned, output, getOrCreateKey());
            return output;
        } finally {
            //noinspection ResultOfMethodCallIgnored
            unsigned.delete();
        }
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
        List<String> mimes = new ArrayList<>();
        for (String item : properties.getProperty("MimeType", "").split(";")) {
            String mime = item.trim();
            if (mime.matches("[A-Za-z0-9!#$&^_.+-]+/[A-Za-z0-9!#$&^_.+-]+") && !mimes.contains(mime)) mimes.add(mime);
        }
        return new DesktopEntry(name, mimes);
    }

    private static void writeUnsigned(File file, String pkg, DesktopEntry entry, String ref,
            byte[] icon, List<String> permissions) throws IOException {
        List<String> cleanPermissions = new ArrayList<>();
        if (permissions != null) for (String p : permissions) {
            if (p == null || !p.matches("[A-Za-z0-9_.]+")) throw new IllegalArgumentException("Invalid Android permission: " + p);
            if (!cleanPermissions.contains(p)) cleanPermissions.add(p);
        }
        byte[] manifest = new BinaryManifest(pkg, entry, ref, cleanPermissions).encode();
        try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(file))) {
            put(zip, "AndroidManifest.xml", manifest);
            // AAPT2 emits this valid empty resource table for a manifest-only package. PackageManager
            // requires resources.arsc to open the manifest, even when the APK has no resources.
            put(zip, "resources.arsc", EMPTY_RESOURCE_TABLE);
            put(zip, "classes.dex", emptyDex());
            put(zip, "assets/matonos-stub/icon.png", icon);
        }
    }

    private static final byte[] EMPTY_RESOURCE_TABLE = new byte[] {
            0x02, 0x00, 0x0c, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x1c, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00
    };

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
                .setV1SigningEnabled(true).setV2SigningEnabled(true).setV3SigningEnabled(true)
                .setV4SigningEnabled(false).build().sign();
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
        byte[] encode() throws IOException {
            for (String s : Arrays.asList(ANDROID, "android", "manifest", "package", "versionCode", "versionName", "uses-sdk", "minSdkVersion", "targetSdkVersion", "uses-permission", "name", "uses-library", "required", "application", "label", "hasCode", "activity", "exported", "meta-data", "value", "intent-filter", "action", "category", "data", "mimeType", "service", "org.matonos.compositor.stub.StubActivity", "org.matonos.compositor.stub.StubService", "android.intent.action.MAIN", "android.intent.category.LAUNCHER", "android.intent.action.VIEW", "android.intent.category.DEFAULT", "android.intent.category.BROWSABLE", HOST_LIBRARY, REF_META, MIN_INTERFACE_META, pkg, label, ref, "1", "1.0", "30", "36")) str(s);
            for (String p : permissions) str(p);
            for (String mime : mimes) str(mime);
            namespace(true);
            start("manifest", null, attrs(a("package", pkg), ai("versionCode", 1), a("versionName", "1.0")));
            start("uses-sdk", null, attrs(ai("minSdkVersion", 30), ai("targetSdkVersion", 36)));
            end("uses-sdk");
            for (String p : permissions) startEnd("uses-permission", attrs(a("name", p)));
            start("application", null, attrs(a("label", label), ab("hasCode", true)));
            startEnd("uses-library", attrs(a("name", HOST_LIBRARY), ab("required", true)));
            start("activity", null, attrs(a("name", HOST_ACTIVITY), ab("exported", true), a("label", label)));
            startEnd("meta-data", attrs(a("name", REF_META), a("value", ref)));
            startEnd("meta-data", attrs(a("name", MIN_INTERFACE_META), ai("value", 1)));
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
            ByteArrayOutputStream body = new ByteArrayOutputStream();
            writeStringPool(body); writeResourceMap(body);
            for (Chunk chunk : nodes) chunk.write(body);
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            write16(out, 0x0003); write16(out, 8); write32(out, 8 + body.size()); body.writeTo(out); return out.toByteArray();
        }
        private int str(String s) { Integer i = strings.get(s); if (i != null) return i; int n = strings.size(); strings.put(s, n); return n; }
        private Attr a(String name, String value) { return new Attr(name, value, 0, 3); }
        private Attr ai(String name, int value) { return new Attr(name, Integer.toString(value), value, 0x10); }
        private Attr ab(String name, boolean value) { return new Attr(name, value ? "true" : "false", value ? 1 : 0, 0x12); }
        private List<Attr> attrs(Attr... a) { return Arrays.asList(a); }
        private void namespace(boolean start) throws IOException { int t = start ? 0x0100 : 0x0101; nodes.add(Chunk.namespace(t, str("android"), str(ANDROID))); }
        private void start(String name, String text, List<Attr> attrs) throws IOException {
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
            for (String name : Arrays.asList("name", "label", "exported", "hasCode", "required", "value", "mimeType", "versionCode", "versionName", "minSdkVersion", "targetSdkVersion")) {
                Integer i = strings.get(name); if (i != null) ids[i] = resourceId(name);
            }
            write16(out, 0x0180); write16(out, 8); write32(out, 8 + ids.length * 4); for (int id : ids) write32(out, id);
        }
        private int resourceId(String name) {
            switch (name) {
                case "name": return 0x01010003; case "label": return 0x01010001; case "exported": return 0x01010010;
                case "hasCode": return 0x0101000c; case "required": return 0x0101028e; case "value": return 0x01010024;
                case "mimeType": return 0x01010026; case "versionCode": return 0x0101021b; case "versionName": return 0x0101021c;
                case "minSdkVersion": return 0x0101020c; case "targetSdkVersion": return 0x01010270; default: return 0;
            }
        }
        private static void write8Length(ByteArrayOutputStream out, int n) { if (n > 127) { out.write((n >> 8) | 0x80); out.write(n & 0xff); } else out.write(n); }
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
                    write32(out, "package".equals(a.name) ? -1 : androidUri); write32(out, nameIndex); write32(out, valueIndex);
                    write16(out, 8); out.write(0); out.write(a.type); write32(out, a.type == 3 ? strings.get(a.value) : a.data);
                }
                return new Chunk(out.toByteArray());
            }
            static Chunk end(int name) throws IOException { ByteArrayOutputStream out = node(0x0103, 24); write32(out, -1); write32(out, name); return new Chunk(out.toByteArray()); }
            private static ByteArrayOutputStream node(int type, int size) throws IOException { ByteArrayOutputStream out = new ByteArrayOutputStream(); write16(out, type); write16(out, 16); write32(out, size); write32(out, 1); write32(out, -1); return out; }
        }
    }
}
