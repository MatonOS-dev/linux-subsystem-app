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
                    app.toFile(), runtime.toFile(), home.toFile(), "/app/share/icons/theme", "direct", 24));
            assertEquals(sized.resolve("sized.svg").toFile(), StubIconLookup.find(
                    app.toFile(), runtime.toFile(), home.toFile(), "/app/share/icons/theme", "sized", 24));
            assertEquals(scalable.resolve("scaled.png").toFile(), StubIconLookup.find(
                    app.toFile(), runtime.toFile(), home.toFile(), "/app/share/icons/theme", "scaled", 24));
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
                    "/app/../../outside"));
            Files.createSymbolicLink(app.resolve("escape"), outside);
            assertNull(StubIconLookup.mapDirectoryUnchecked(app.toFile(), runtime.toFile(), home.toFile(),
                    "/app/escape"));
            assertNull(StubIconLookup.mapDirectoryUnchecked(app.toFile(), runtime.toFile(), home.toFile(),
                    outside.resolve("icons").toString()));
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
