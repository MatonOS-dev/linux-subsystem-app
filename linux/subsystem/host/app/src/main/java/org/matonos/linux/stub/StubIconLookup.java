package org.matonos.linux.stub;

import android.util.Log;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

/** Resolves SNI icon files through this stub's existing Flatpak deployment roots. */
public final class StubIconLookup {
    private static final String TAG = "MatonStubIcons";

    private StubIconLookup() {}

    public static File find(File appFiles, File runtimeFiles, File home, String themePath,
            String iconName, int sizePx) {
        if (sizePx <= 0 || iconName == null || !iconName.matches("[A-Za-z0-9_.+-]+")) {
            Log.e(TAG, "Rejected invalid icon request path=" + themePath + " name=" + iconName
                    + " size=" + sizePx);
            return null;
        }
        try {
            File directory = mapDirectory(appFiles, runtimeFiles, home, themePath);
            if (directory == null || !directory.isDirectory()) {
                Log.e(TAG, "Icon theme directory unavailable path=" + themePath);
                return null;
            }
            List<File> candidates = new ArrayList<>();
            addVariants(candidates, directory, iconName);
            addVariants(candidates, new File(directory, sizePx + "x" + sizePx + "/apps"), iconName);
            addVariants(candidates, new File(directory, "scalable/apps"), iconName);
            for (File candidate : candidates) {
                File canonical = candidate.getCanonicalFile();
                if (!insideAllowedRoots(canonical, appFiles, runtimeFiles, home)) {
                    Log.e(TAG, "Icon candidate escapes owning stub trees path=" + candidate
                            + " canonical=" + canonical);
                    continue;
                }
                if (canonical.isFile() && canonical.canRead()) return canonical;
            }
            Log.e(TAG, "Icon not found path=" + themePath + " name=" + iconName + " size=" + sizePx);
            return null;
        } catch (IOException error) {
            Log.e(TAG, "Icon lookup failed path=" + themePath + " name=" + iconName
                    + " size=" + sizePx, error);
            return null;
        }
    }

    static File mapDirectory(File appFiles, File runtimeFiles, File home, String path) throws IOException {
        File mapped = mapDirectoryUnchecked(appFiles, runtimeFiles, home, path);
        if (mapped == null && path != null && !path.isEmpty())
            Log.e(TAG, "Rejected icon theme path outside the owning app trees path=" + path);
        return mapped;
    }

    static File mapDirectoryUnchecked(File appFiles, File runtimeFiles, File home, String path) throws IOException {
        if (path == null || path.isEmpty() || !path.startsWith("/")) return null;
        File root;
        String relative;
        if (path.equals("/app") || path.startsWith("/app/")) {
            root = appFiles;
            relative = path.length() == 4 ? "" : path.substring(5);
        } else if (path.equals("/usr") || path.startsWith("/usr/")) {
            root = runtimeFiles;
            relative = path.length() == 4 ? "" : path.substring(5);
        } else {
            File canonicalHome = home.getCanonicalFile();
            File candidate = new File(path).getCanonicalFile();
            if (inside(candidate.toPath(), canonicalHome.toPath())) return candidate;
            return null;
        }
        if (root == null) return null;
        File canonicalRoot = root.getCanonicalFile();
        File candidate = new File(canonicalRoot, relative).getCanonicalFile();
        if (!inside(candidate.toPath(), canonicalRoot.toPath())) {
            return null;
        }
        return candidate;
    }

    private static boolean insideAllowedRoots(File file, File app, File runtime, File home) throws IOException {
        Path path = file.toPath();
        return app != null && inside(path, app.getCanonicalFile().toPath())
                || runtime != null && inside(path, runtime.getCanonicalFile().toPath())
                || home != null && inside(path, home.getCanonicalFile().toPath());
    }

    private static boolean inside(Path child, Path root) {
        return child.equals(root) || child.startsWith(root);
    }

    private static void addVariants(List<File> out, File directory, String name) {
        out.add(new File(directory, name + ".png"));
        out.add(new File(directory, name + ".svg"));
    }
}
