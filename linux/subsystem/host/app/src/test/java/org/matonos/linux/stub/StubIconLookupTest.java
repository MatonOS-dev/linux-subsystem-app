package org.matonos.linux.stub;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;
import java.nio.file.Files;
import java.nio.file.Path;
import org.junit.Test;

public final class StubIconLookupTest {
    @Test public void lookupChecksDirectoryThenSizedAppsThenScalableAndPngBeforeSvg() throws Exception {
        Path app = Files.createTempDirectory("stub-icons-app");
        Path runtime = Files.createTempDirectory("stub-icons-runtime");
        Path home = Files.createTempDirectory("stub-icons-home");
        try {
            Path base = Files.createDirectories(app.resolve("share/icons/theme"));
            Path sized = Files.createDirectories(base.resolve("24x24/apps"));
            Path scalable = Files.createDirectories(base.resolve("scalable/apps"));
            Files.write(base.resolve("direct.svg"), new byte[]{1});
            Files.write(base.resolve("direct.png"), new byte[]{1});
            Files.write(sized.resolve("sized.svg"), new byte[]{1});
            Files.write(scalable.resolve("scaled.png"), new byte[]{1});

            assertEquals(base.resolve("direct.png").toFile(), StubIconLookup.find(
                    app.toFile(), runtime.toFile(), home.toFile(), "org.example.App",
                    "/app/share/icons/theme", "direct", 24));
            assertEquals(sized.resolve("sized.svg").toFile(), StubIconLookup.find(
                    app.toFile(), runtime.toFile(), home.toFile(), "org.example.App",
                    "/app/share/icons/theme", "sized", 24));
            assertEquals(scalable.resolve("scaled.png").toFile(), StubIconLookup.find(
                    app.toFile(), runtime.toFile(), home.toFile(), "org.example.App",
                    "/app/share/icons/theme", "scaled", 24));
        } finally {
            delete(app); delete(runtime); delete(home);
        }
    }

    @Test public void mapsFlatpakVarDataToTheOwningAppsExistingPersistentDirectory() throws Exception {
        Path app = Files.createTempDirectory("stub-icons-app");
        Path runtime = Files.createTempDirectory("stub-icons-runtime");
        Path home = Files.createTempDirectory("stub-icons-home");
        try {
            Path steamIcons = Files.createDirectories(home.resolve(
                    ".var/app/com.valvesoftware.Steam/data/Steam/public/24x24/apps"));
            Path icon = steamIcons.resolve("steam_tray_mono.png");
            Files.write(icon, new byte[]{1});

            assertEquals(icon.toFile(), StubIconLookup.find(app.toFile(), runtime.toFile(), home.toFile(),
                    "com.valvesoftware.Steam", "/var/data/Steam/public", "steam_tray_mono", 24));
            assertEquals(home.resolve(".var/app/com.valvesoftware.Steam/config/Steam").toFile(),
                    StubIconLookup.mapDirectoryUnchecked(app.toFile(), runtime.toFile(), home.toFile(),
                            "com.valvesoftware.Steam", "/var/config/Steam"));
            assertEquals(home.resolve(".var/app/com.valvesoftware.Steam/cache/Steam").toFile(),
                    StubIconLookup.mapDirectoryUnchecked(app.toFile(), runtime.toFile(), home.toFile(),
                            "com.valvesoftware.Steam", "/var/cache/Steam"));
            assertEquals(home.resolve(".var/app/com.valvesoftware.Steam/tmp").toFile(),
                    StubIconLookup.mapDirectoryUnchecked(app.toFile(), runtime.toFile(), home.toFile(),
                            "com.valvesoftware.Steam", "/var/tmp"));
            assertNull(StubIconLookup.mapDirectoryUnchecked(app.toFile(), runtime.toFile(), home.toFile(),
                    "com.valvesoftware.Steam", "/var/database/Steam"));
        } finally {
            delete(app); delete(runtime); delete(home);
        }
    }

    @Test public void rejectsTraversalAndSymlinkEscapesFromAppAndHome() throws Exception {
        Path app = Files.createTempDirectory("stub-icons-app");
        Path runtime = Files.createTempDirectory("stub-icons-runtime");
        Path home = Files.createTempDirectory("stub-icons-home");
        Path outside = Files.createTempDirectory("stub-icons-outside");
        try {
            assertNull(StubIconLookup.mapDirectoryUnchecked(app.toFile(), runtime.toFile(), home.toFile(),
                    "org.example.App", "/app/../../outside"));
            Files.createSymbolicLink(app.resolve("escape"), outside);
            assertNull(StubIconLookup.mapDirectoryUnchecked(app.toFile(), runtime.toFile(), home.toFile(),
                    "org.example.App", "/app/escape"));
            assertNull(StubIconLookup.mapDirectoryUnchecked(app.toFile(), runtime.toFile(), home.toFile(),
                    "org.example.App", outside.resolve("icons").toString()));
        } finally {
            delete(app); delete(runtime); delete(home); delete(outside);
        }
    }

    private static void delete(Path root) throws Exception {
        if (!Files.exists(root)) return;
        try (java.util.stream.Stream<Path> paths = Files.walk(root)) {
            paths.sorted(java.util.Comparator.reverseOrder()).forEach(path -> path.toFile().delete());
        }
    }
}
