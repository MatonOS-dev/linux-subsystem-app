package org.matonos.linux;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

/** Freedesktop icon theme lookup over an installed extension's read-only files. */
public final class IconThemeLookup {
    private IconThemeLookup() {}

    public static List<File> themes(File iconRoot) throws IOException {
        return themes(java.util.Collections.singletonList(iconRoot));
    }

    public static List<File> themes(List<File> iconRoots) throws IOException {
        List<File> result = new ArrayList<>();
        for (File iconRoot : iconRoots) {
            File[] children = iconRoot.listFiles();
            if (children == null) {
                if (!iconRoot.exists()) continue;
                throw new IOException("Cannot enumerate icon directory: " + iconRoot);
            }
            for (File child : children) {
                if (child.isDirectory() && new File(child, "index.theme").isFile()) result.add(child);
            }
        }
        return result;
    }

    public static File find(File iconRoot, String theme, String name, int sizePx, int scale)
            throws IOException {
        return find(java.util.Collections.singletonList(iconRoot), theme, name, sizePx, scale);
    }

    public static File find(List<File> iconRoots, String theme, String name, int sizePx, int scale)
            throws IOException {
        if (!safe(theme) || !safe(name) || sizePx <= 0 || scale <= 0) return null;
        List<File> themes = themes(iconRoots);
        Map<String, File> byName = new LinkedHashMap<>();
        for (File directory : themes) byName.put(directory.getName(), directory);
        if (!byName.containsKey(theme)) return null;

        List<String> chain = new ArrayList<>();
        collectTheme(theme, byName, chain, new HashSet<>());
        if (!chain.contains("hicolor") && byName.containsKey("hicolor")) chain.add("hicolor");
        String[] names = name.endsWith("-symbolic") ? new String[]{name} : new String[]{name + "-symbolic", name};
        for (String iconName : names) {
            for (String themeName : chain) {
                File dir = byName.get(themeName);
                if (dir == null) continue;
                ThemeIndex index = ThemeIndex.read(new File(dir, "index.theme"));
                for (ThemeIndex.IconDirectory candidate : index.directories(sizePx, scale)) {
                    for (String suffix : new String[]{".png", ".svg"}) {
                        File file = new File(new File(dir, candidate.path), iconName + suffix);
                        if (file.isFile()) return file;
                    }
                }
            }
        }
        return null;
    }

    private static boolean safe(String value) {
        return value != null && !value.isEmpty() && value.matches("[A-Za-z0-9_.+-]+");
    }

    private static void collectTheme(String name, Map<String, File> themes, List<String> result,
            Set<String> visited) throws IOException {
        if (!visited.add(name)) return;
        File directory = themes.get(name);
        if (directory == null) return;
        result.add(name);
        ThemeIndex index = ThemeIndex.read(new File(directory, "index.theme"));
        for (String inherited : index.inherits) collectTheme(inherited, themes, result, visited);
    }

    static final class ThemeIndex {
        final List<String> inherits = new ArrayList<>();
        final Map<String, Map<String, String>> sections = new LinkedHashMap<>();

        static ThemeIndex read(File file) throws IOException {
            if (!file.isFile()) throw new IOException("Missing index.theme: " + file);
            ThemeIndex result = new ThemeIndex();
            String section = "";
            for (String raw : Files.readAllLines(file.toPath(), StandardCharsets.UTF_8)) {
                String line = raw.trim();
                if (line.isEmpty() || line.startsWith("#") || line.startsWith(";")) continue;
                if (line.startsWith("[") && line.endsWith("]")) {
                    section = line.substring(1, line.length() - 1);
                    result.sections.put(section, new LinkedHashMap<>());
                } else {
                    int equal = line.indexOf('=');
                    if (equal <= 0 || section.isEmpty()) throw new IOException("Malformed index.theme line in " + file + ": " + raw);
                    result.sections.get(section).put(line.substring(0, equal).trim(), line.substring(equal + 1).trim());
                }
            }
            Map<String, String> main = result.sections.get("Icon Theme");
            if (main == null) throw new IOException("Missing [Icon Theme] section in " + file);
            String inherit = main.get("Inherits");
            if (inherit != null) for (String value : inherit.split(",")) if (!value.trim().isEmpty()) result.inherits.add(value.trim());
            return result;
        }

        List<IconDirectory> directories(int requested, int requestedScale) throws IOException {
            Map<String, String> main = sections.get("Icon Theme");
            String declared = main.get("Directories");
            if (declared == null) throw new IOException("Missing Directories in [Icon Theme]");
            List<IconDirectory> result = new ArrayList<>();
            for (String rawPath : declared.split(",")) {
                String path = rawPath.trim();
                if (path.isEmpty() || path.startsWith("/") || path.contains("..")) throw new IOException("Invalid icon directory path: " + path);
                Map<String, String> values = sections.get(path);
                if (values == null) throw new IOException("Missing directory section [" + path + "]");
                int size = integer(values, "Size", 0);
                int min = integer(values, "MinSize", size);
                int max = integer(values, "MaxSize", size);
                int threshold = integer(values, "Threshold", 2);
                int dirScale = integer(values, "Scale", 1);
                String type = values.getOrDefault("Type", "Threshold");
                int target = requested / requestedScale;
                int distance;
                if (type.equals("Scalable")) distance = target < min ? min - target : target > max ? target - max : 0;
                else if (type.equals("Fixed")) distance = Math.abs(target - size);
                else distance = target < size - threshold ? size - threshold - target : target > size + threshold ? target - size - threshold : 0;
                result.add(new IconDirectory(path, Math.abs(requestedScale - dirScale), distance));
            }
            result.sort((left, right) -> {
                int scaleOrder = Integer.compare(left.scaleDistance, right.scaleDistance);
                return scaleOrder != 0 ? scaleOrder : Integer.compare(left.distance, right.distance);
            });
            return result;
        }

        private static int integer(Map<String, String> values, String key, int fallback) throws IOException {
            String value = values.get(key);
            if (value == null) return fallback;
            try { return Integer.parseInt(value); }
            catch (NumberFormatException error) { throw new IOException("Invalid " + key + " in index.theme: " + value, error); }
        }

        static final class IconDirectory {
            final String path;
            final int scaleDistance;
            final int distance;
            IconDirectory(String path, int scaleDistance, int distance) {
                this.path = path;
                this.scaleDistance = scaleDistance;
                this.distance = distance;
            }
        }
    }
}
