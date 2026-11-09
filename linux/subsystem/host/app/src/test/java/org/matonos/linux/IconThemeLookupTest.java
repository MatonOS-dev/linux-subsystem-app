package org.matonos.linux;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import org.junit.Test;

public final class IconThemeLookupTest {
    @Test public void resolvesDirectorySizesScalableThresholdInheritanceAndSymbolic() throws Exception {
        Path root = Files.createTempDirectory("icon-theme-test");
        Path runtimeRoot = Files.createTempDirectory("icon-runtime-test");
        try {
            Path papirus = theme(root, "Papirus", "Inherits=Base,hicolor\nDirectories=16x16/apps,scalable/apps,22x22/apps,24x24@2/apps\n",
                    "[16x16/apps]\nSize=16\nType=Fixed\n",
                    "[scalable/apps]\nSize=32\nMinSize=16\nMaxSize=64\nType=Scalable\n",
                    "[22x22/apps]\nSize=22\nThreshold=2\nType=Threshold\n",
                    "[24x24@2/apps]\nSize=24\nScale=2\nType=Fixed\n");
            Path base = theme(root, "Base", "Inherits=hicolor\nDirectories=apps\n", "[apps]\nSize=24\nType=Fixed\n");
            Path hicolor = theme(runtimeRoot, "hicolor", "Directories=apps\n", "[apps]\nSize=32\nType=Fixed\n");
            touch(papirus.resolve("16x16/apps/tool.png"));
            touch(papirus.resolve("16x16/apps/tool-symbolic.svg"));
            touch(papirus.resolve("scalable/apps/vector.svg"));
            touch(papirus.resolve("22x22/apps/threshold.png"));
            touch(papirus.resolve("24x24@2/apps/highdpi.png"));
            touch(base.resolve("apps/inherited.png"));
            touch(hicolor.resolve("apps/fallback.png"));

            assertEquals(papirus.resolve("16x16/apps/tool-symbolic.svg").toFile(),
                    IconThemeLookup.find(root.toFile(), "Papirus", "tool", 16, 1));
            assertEquals(papirus.resolve("scalable/apps/vector.svg").toFile(),
                    IconThemeLookup.find(root.toFile(), "Papirus", "vector", 48, 1));
            assertEquals(papirus.resolve("22x22/apps/threshold.png").toFile(),
                    IconThemeLookup.find(root.toFile(), "Papirus", "threshold", 24, 1));
            assertEquals(papirus.resolve("24x24@2/apps/highdpi.png").toFile(),
                    IconThemeLookup.find(root.toFile(), "Papirus", "highdpi", 48, 2));
            assertEquals(base.resolve("apps/inherited.png").toFile(),
                    IconThemeLookup.find(root.toFile(), "Papirus", "inherited", 24, 1));
            assertEquals(hicolor.resolve("apps/fallback.png").toFile(),
                    IconThemeLookup.find(java.util.Arrays.asList(root.toFile(), runtimeRoot.toFile()),
                            "Papirus", "fallback", 32, 1));
            assertNull(IconThemeLookup.find(root.toFile(), "Missing", "tool", 16, 1));
            assertNotNull(IconThemeLookup.find(root.toFile(), "Papirus", "tool", 20, 1));
        } finally {
            delete(root);
            delete(runtimeRoot);
        }
    }

    private static Path theme(Path root, String name, String main, String... directories) throws Exception {
        Path dir = Files.createDirectories(root.resolve(name));
        StringBuilder contents = new StringBuilder("[Icon Theme]\n").append(main);
        for (String directory : directories) contents.append(directory);
        Files.write(dir.resolve("index.theme"), contents.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
        return dir;
    }

    private static void touch(Path path) throws Exception {
        Files.createDirectories(path.getParent());
        Files.write(path, new byte[]{1});
    }

    private static void delete(Path path) throws Exception {
        if (!Files.exists(path)) return;
        try (java.util.stream.Stream<Path> paths = Files.walk(path)) {
            paths.sorted(java.util.Comparator.reverseOrder()).forEach(item -> item.toFile().delete());
        }
    }
}
