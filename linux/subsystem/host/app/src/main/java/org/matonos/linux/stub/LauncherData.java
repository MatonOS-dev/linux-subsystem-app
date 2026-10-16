package org.matonos.linux.stub;

import org.json.JSONObject;
import org.matonos.linuxhost.stubgen.StubGenerator;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/**
 * The launcher label, MIME associations and icon for one package's code-free
 * launcher split.
 *
 * <p>Source order (v3.5):
 *
 * <ol>
 *   <li>the applied extra output's {@code export/} when {@code apply_extra}
 *       produced a {@code .desktop} there (it overlays the deployment's own
 *       export, and the components then live in the {@code extra_} split);</li>
 *   <li>the deployment's own {@code export/share/applications/*.desktop};</li>
 *   <li>the remote's appstream entry, read here, never passed in by a
 *       caller;</li>
 *   <li>the Flatpak app id and a generic icon supplied by the caller.</li>
 * </ol>
 *
 * <p>No Android dependency: the caller injects the appstream reader so the
 * decision logic is unit-testable on the JVM.
 */
public final class LauncherData {
    /** Largest launcher icon accepted (upstream's own limit). */
    public static final int MAX_ICON_BYTES = 1024 * 1024;

    public final String label;
    public final List<String> mimeTypes;
    /** PNG bytes, or null meaning "use the caller's generic icon". */
    public final byte[] icon;
    /** True when the {@code .desktop} came from the extra output: the launcher
     * components must then be carried by the {@code extra_} split. */
    public final boolean fromExtra;

    private LauncherData(String label, List<String> mimeTypes, byte[] icon, boolean fromExtra) {
        this.label = label;
        this.mimeTypes = mimeTypes;
        this.icon = icon;
        this.fromExtra = fromExtra;
    }

    /** Appstream lookup, injected so tests need no network. */
    public interface Appstream {
        /** Appstream name and PNG icon for an app id, or null. */
        Entry lookup(String appId) throws Exception;
        final class Entry {
            public final String name;
            public final byte[] icon;
            public Entry(String name, byte[] icon) { this.name = name; this.icon = validIcon(icon); }
        }
    }

    /** The production appstream reader: Flathub's v2 API. */
    public static final class Flathub implements Appstream {
        private static final String API = "https://flathub.org/api/v2/appstream/";

        @Override public Entry lookup(String appId) {
            try {
                byte[] body = download(API + appId, "application/json", 256 * 1024);
                if (body == null) return null;
                JSONObject json = new JSONObject(new String(body, StandardCharsets.UTF_8));
                String name = json.optString("name", "").trim();
                if (name.isEmpty()) return null;
                String icon = json.optString("icon", "");
                byte[] png = null;
                if (looksLikeUrl(icon)) png = download(icon, "image/png,image/*", MAX_ICON_BYTES);
                return new Entry(name, png);
            } catch (Exception unavailable) {
                return null;
            }
        }

        private static byte[] download(String url, String accept, int limit) throws IOException {
            HttpURLConnection connection = (HttpURLConnection) new URL(url).openConnection();
            connection.setConnectTimeout(10_000);
            connection.setReadTimeout(20_000);
            connection.setInstanceFollowRedirects(true);
            connection.setRequestProperty("Accept", accept);
            try {
                if (connection.getResponseCode() != 200) return null;
                try (InputStream in = connection.getInputStream()) {
                    ByteArrayOutputStream out = new ByteArrayOutputStream();
                    byte[] buffer = new byte[8192];
                    int total = 0, read;
                    while ((read = in.read(buffer)) != -1) {
                        total += read;
                        if (total > limit) return null;
                        out.write(buffer, 0, read);
                    }
                    return out.toByteArray();
                }
            } finally {
                connection.disconnect();
            }
        }

        private static boolean looksLikeUrl(String value) {
            return value != null && value.startsWith("https://") && value.length() <= 2048;
        }
    }

    /**
     * Resolves the launcher data. {@code deployment} is the staged app
     * deployment; {@code extraOutput} is the extracted {@code apply_extra}
     * result, or null; {@code appstream} is the injected remote lookup, or
     * null to skip it.
     */
    public static LauncherData resolve(File deployment, String appId, File extraOutput,
            Appstream appstream) throws Exception {
        LauncherData extra = fromExport(extraOutput, appId, true);
        if (extra != null) return extra;
        LauncherData deployed = fromExport(deployment, appId, false);
        if (deployed != null) return deployed;
        if (appstream != null) {
            Appstream.Entry entry = appstream.lookup(appId);
            if (entry != null && entry.name != null && !entry.name.trim().isEmpty())
                return new LauncherData(entry.name, Collections.<String>emptyList(), entry.icon, false);
        }
        return new LauncherData(appId, Collections.<String>emptyList(), null, false);
    }

    /**
     * The launcher data of an applied extra output's {@code export/}, or null
     * when it carries no {@code .desktop}. {@link #fromExtra} is then true: the
     * components must be carried by the {@code extra_} split, because the
     * extra output is installed after the launcher decision.
     */
    public static LauncherData fromExtraOutput(File extraOutput, String appId) throws IOException {
        return fromExport(extraOutput, appId, true);
    }

    /** The export tree of {@code root}, or null when it carries no .desktop. */
    private static LauncherData fromExport(File root, String appId, boolean fromExtra) throws IOException {
        if (root == null) return null;
        File desktop = desktopFile(new File(root, "export/share/applications"), appId);
        if (desktop == null) return null;
        byte[] content = Files.readAllBytes(desktop.toPath());
        if (content.length == 0 || content.length > 1024 * 1024) throw new IOException("Desktop entry is too large");
        StubGenerator.DesktopEntry entry = StubGenerator.parseDesktop(new String(content, StandardCharsets.UTF_8));
        byte[] icon = exportIcon(root, appId, entry.iconName);
        return new LauncherData(entry.name, entry.mimeTypes, icon, fromExtra);
    }

    /** The app's exported .desktop: the app id name first, else a lone file. */
    private static File desktopFile(File directory, String appId) {
        if (!directory.isDirectory() || Files.isSymbolicLink(directory.toPath())) return null;
        File exact = new File(directory, appId + ".desktop");
        if (regular(exact)) return exact;
        File[] files = directory.listFiles();
        if (files == null) return null;
        List<File> desktops = new ArrayList<>();
        for (File file : files) if (regular(file) && file.getName().endsWith(".desktop")) desktops.add(file);
        return desktops.size() == 1 ? desktops.get(0) : null;
    }

    /** A PNG from {@code export/share/icons/hicolor}, preferring the .desktop's
     * Icon= name and the largest size, else any app PNG. Never a symlink. */
    private static byte[] exportIcon(File root, String appId, String iconName) throws IOException {
        File hicolor = new File(root, "export/share/icons/hicolor");
        if (!hicolor.isDirectory()) return null;
        File[] sizes = hicolor.listFiles();
        if (sizes == null) return null;
        Arrays.sort(sizes, Comparator.comparingInt(LauncherData::iconSize).reversed()
                .thenComparing(File::getName));
        List<String> names = new ArrayList<>();
        if (iconName != null && !iconName.isEmpty()) names.add(iconName);
        if (!names.contains(appId)) names.add(appId);
        for (String name : names)
            for (File size : sizes) {
                byte[] icon = readPng(new File(new File(size, "apps"), name + ".png"));
                if (icon != null) return icon;
            }
        for (File size : sizes) {
            File apps = new File(size, "apps");
            File[] files = apps.listFiles();
            if (files == null) continue;
            Arrays.sort(files, Comparator.comparing(File::getName));
            for (File file : files) {
                byte[] icon = readPng(file);
                if (icon != null) return icon;
            }
        }
        return null;
    }

    /** Numeric leading size ("128x128" -> 128); "scalable" sorts last. */
    private static int iconSize(File directory) {
        String name = directory.getName();
        int end = 0;
        while (end < name.length() && Character.isDigit(name.charAt(end))) end++;
        if (end == 0) return -1;
        try { return Integer.parseInt(name.substring(0, end)); }
        catch (NumberFormatException tooBig) { return Integer.MAX_VALUE; }
    }

    private static byte[] readPng(File file) throws IOException {
        if (!regular(file)) return null;
        long length = file.length();
        if (length <= 0 || length > MAX_ICON_BYTES) return null;
        byte[] bytes = new byte[(int) length];
        try (InputStream in = Files.newInputStream(file.toPath())) {
            int read = 0;
            while (read < bytes.length) {
                int count = in.read(bytes, read, bytes.length - read);
                if (count < 0) return null;
                read += count;
            }
        }
        return validIcon(bytes);
    }

    /** PNG bytes within the limit, or null. */
    public static byte[] validIcon(byte[] bytes) {
        if (bytes == null || bytes.length < 8 || bytes.length > MAX_ICON_BYTES) return null;
        byte[] magic = {(byte) 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
        for (int i = 0; i < magic.length; i++) if (bytes[i] != magic[i]) return null;
        return bytes;
    }

    private static boolean regular(File file) {
        return !Files.isSymbolicLink(file.toPath()) && Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS);
    }
}
