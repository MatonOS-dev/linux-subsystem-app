package org.matonos.linux.stub;

import org.json.JSONObject;
import org.matonos.linuxhost.stubgen.PlaceholderFiles;
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
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import com.caverock.androidsvg.SVG;

/**
 * The launcher label, MIME associations and icon for one package's code-free
 * launcher split.
 *
 * <p>Source order (v3.5):
 *
 * <ol>
 *   <li>the deployed {@code files/extra/export/} tree, which stock Flatpak
 *       populated during deployment;</li>
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
    public final String label;
    public final List<String> mimeTypes;
    /** PNG bytes, or null meaning "use the caller's generic icon". */
    public final byte[] icon;
    private LauncherData(String label, List<String> mimeTypes, byte[] icon) {
        this.label = label;
        this.mimeTypes = mimeTypes;
        this.icon = icon;
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
                byte[] body = download(API + appId, "application/json");
                if (body == null) return null;
                JSONObject json = new JSONObject(new String(body, StandardCharsets.UTF_8));
                String name = json.optString("name", "").trim();
                if (name.isEmpty()) return null;
                String icon = json.optString("icon", "");
                byte[] png = null;
                if (looksLikeUrl(icon)) png = download(icon, "image/png,image/*");
                return new Entry(name, png);
            } catch (Exception unavailable) {
                return null;
            }
        }

        private static byte[] download(String url, String accept) throws IOException {
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
                    int read;
                    while ((read = in.read(buffer)) != -1) {
                        if (out.size() > Integer.MAX_VALUE - read)
                            throw new IOException("Appstream response is too large for an in-memory result");
                        out.write(buffer, 0, read);
                    }
                    return out.toByteArray();
                }
            } finally {
                connection.disconnect();
            }
        }

        private static boolean looksLikeUrl(String value) {
            return value != null && value.startsWith("https://");
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
        LauncherData extra = fromExport(extraOutput, appId);
        if (extra != null) return extra;
        LauncherData deployed = fromExport(deployment, appId);
        if (deployed != null) return deployed;
        if (appstream != null) {
            Appstream.Entry entry = appstream.lookup(appId);
            if (entry != null && entry.name != null && !entry.name.trim().isEmpty())
                return new LauncherData(entry.name, Collections.<String>emptyList(), entry.icon);
        }
        return new LauncherData(appId, Collections.<String>emptyList(), null);
    }

    /**
     * Launcher data from the applied extra tree's {@code export/}, or null
     * when it carries no {@code .desktop}.
     */
    public static LauncherData fromExtraOutput(File extraOutput, String appId) throws IOException {
        return fromExport(extraOutput, appId);
    }

    /** The export tree of {@code root}, or null when it carries no .desktop. */
    private static LauncherData fromExport(File root, String appId) throws IOException {
        if (root == null) return null;
        File desktop = desktopFile(new File(root, "export/share/applications"), appId);
        if (desktop == null) {
            byte[] icon = exportIcon(root, appId, null);
            return icon == null ? null : new LauncherData(appId, Collections.<String>emptyList(), icon);
        }
        byte[] content = PlaceholderFiles.readAllBytes(desktop);
        if (content.length == 0) throw new IOException("Desktop entry is empty");
        StubGenerator.DesktopEntry entry = StubGenerator.parseDesktop(new String(content, StandardCharsets.UTF_8));
        byte[] icon = exportIcon(root, appId, entry.iconName);
        return new LauncherData(entry.name, entry.mimeTypes, icon);
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

    /** Exported PNG/SVG from hicolor, preferring Icon= and then the Flatpak id.
     * Theme directories (including scalable and symbolic) are searched after
     * sized directories. Never follows a symlink. */
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
        for (String name : names) {
            for (File size : sizes) {
                File apps = new File(size, "apps");
                for (String extension : new String[]{".png", ".svg"}) {
                    byte[] icon = readIcon(new File(apps, name + extension));
                    if (icon != null) return icon;
                }
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

    private static byte[] readIcon(File file) throws IOException {
        if (!regular(file)) return null;
        long length = PlaceholderFiles.size(file);
        if (length <= 0 || length > Integer.MAX_VALUE) return null;
        byte[] bytes = new byte[(int) length];
        try (InputStream in = PlaceholderFiles.open(file)) {
            int read = 0;
            while (read < bytes.length) {
                int count = in.read(bytes, read, bytes.length - read);
                if (count < 0) return null;
                read += count;
            }
        }
        if (validIcon(bytes) != null) return bytes;
        if (!file.getName().endsWith(".svg")) return null;
        try {
            SVG svg = SVG.getFromString(new String(bytes, StandardCharsets.UTF_8));
            float width = svg.getDocumentWidth(), height = svg.getDocumentHeight();
            if (!(width > 0) || !(height > 0)) { width = 512; height = 512; }
            float scale = Math.min(512f / width, 512f / height);
            Bitmap bitmap = Bitmap.createBitmap(512, 512, Bitmap.Config.ARGB_8888);
            Canvas canvas = new Canvas(bitmap);
            canvas.drawColor(Color.TRANSPARENT);
            canvas.save();
            canvas.translate((512f - width * scale) / 2f, (512f - height * scale) / 2f);
            canvas.scale(scale, scale);
            svg.renderToCanvas(canvas);
            canvas.restore();
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            bitmap.compress(Bitmap.CompressFormat.PNG, 100, out);
            bitmap.recycle();
            return out.toByteArray();
        } catch (Exception invalidSvg) { return null; }
    }

    /** PNG bytes, or null when absent or malformed. */
    public static byte[] validIcon(byte[] bytes) {
        if (bytes == null || bytes.length < 8) return null;
        byte[] magic = {(byte) 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
        for (int i = 0; i < magic.length; i++) if (bytes[i] != magic[i]) return null;
        return bytes;
    }

    /** Pads artwork into the adaptive icon safe zone on a transparent canvas. */
    public static byte[] insetIcon(byte[] bytes) {
        if (bytes == null) return null;
        Bitmap source = BitmapFactory.decodeByteArray(bytes, 0, bytes.length);
        if (source == null) return bytes;
        Bitmap target = Bitmap.createBitmap(512, 512, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(target);
        canvas.drawColor(Color.TRANSPARENT);
        float scale = Math.min(342f / source.getWidth(), 342f / source.getHeight());
        int width = Math.max(1, Math.round(source.getWidth() * scale));
        int height = Math.max(1, Math.round(source.getHeight() * scale));
        int left = (512 - width) / 2, top = (512 - height) / 2;
        android.graphics.Rect dst = new android.graphics.Rect(left, top, left + width, top + height);
        canvas.drawBitmap(source, null, dst, null);
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        target.compress(Bitmap.CompressFormat.PNG, 100, out);
        source.recycle(); target.recycle();
        return out.toByteArray();
    }

    private static boolean regular(File file) {
        return !Files.isSymbolicLink(file.toPath()) && Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS);
    }
}
