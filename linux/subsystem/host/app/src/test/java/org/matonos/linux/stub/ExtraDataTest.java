package org.matonos.linux.stub;

import static org.junit.Assert.*;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import org.junit.Test;

/** Host tests for the deferred extra-data decisions that need no Android API. */
public final class ExtraDataTest {
    private static final String COMMIT =
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    private static File temporary() throws Exception {
        return Files.createTempDirectory("maton-extra").toFile();
    }

    @Test public void readsAndRejectsTheActiveCommitPointer() throws Exception {
        File app = temporary();
        Files.createSymbolicLink(new File(app, "active").toPath(), Paths.get(COMMIT));
        assertEquals(COMMIT, ExtraData.activeCommit(app));
        new File(app, "active").delete();
        assertNull("A missing pointer claims no extra data", ExtraData.activeCommit(app));
        Files.createSymbolicLink(new File(app, "active").toPath(), Paths.get("../escape"));
        assertNull("A non-commit pointer is unusable", ExtraData.activeCommit(app));
        new File(app, "active").delete();
        Files.write(new File(app, "active").toPath(), COMMIT.getBytes(StandardCharsets.UTF_8));
        assertNull("A regular file is not a pointer", ExtraData.activeCommit(app));
    }

    @Test public void findsOnlyPopulatedUnpackedExtra() throws Exception {
        File deployment = temporary();
        assertNull("No files/extra", ExtraData.unpacked(deployment));
        assertTrue(new File(deployment, "files/extra").mkdirs());
        assertNull("Empty signed extra", ExtraData.unpacked(deployment));
        Files.write(new File(deployment, "files/extra/chrome.deb").toPath(), new byte[]{1, 2, 3});
        assertEquals(new File(deployment, "files/extra"), ExtraData.unpacked(deployment));
    }

    @Test public void recordsThePreparedCommit() throws Exception {
        File state = new File(temporary(), "extra.state");
        assertNull("No marker yet", ExtraData.recorded(state));
        Files.write(state.toPath(), (COMMIT + "\n").getBytes(StandardCharsets.UTF_8));
        assertEquals(COMMIT, ExtraData.recorded(state));
        Files.delete(state.toPath());
        Files.createSymbolicLink(state.toPath(), Paths.get("/etc/passwd"));
        assertNull("A link is not a marker", ExtraData.recorded(state));
    }

    @Test public void copiesFilesDirectoriesAndSafeLinks() throws Exception {
        File source = temporary(), destination = new File(temporary(), "extra");
        new File(source, "nested").mkdirs();
        Files.write(new File(source, "chrome.deb").toPath(), new byte[]{7, 8, 9});
        Files.write(new File(source, "nested/opt").toPath(), "bin".getBytes(StandardCharsets.UTF_8));
        Files.createSymbolicLink(new File(source, "link").toPath(), Paths.get("nested/opt"));
        ExtraData.copy(source, destination);
        assertArrayEquals(new byte[]{7, 8, 9}, Files.readAllBytes(new File(destination, "chrome.deb").toPath()));
        assertEquals("bin", new String(Files.readAllBytes(new File(destination, "nested/opt").toPath()),
                StandardCharsets.UTF_8));
        assertEquals(Paths.get("nested/opt"),
                Files.readSymbolicLink(new File(destination, "link").toPath()));
        assertTrue("Copied downloads stay writable for the script",
                new File(destination, "chrome.deb").canWrite());
    }

    @Test public void rejectsLinksThatEscapeTheirTree() throws Exception {
        File source = temporary(), destination = new File(temporary(), "extra");
        Files.createSymbolicLink(new File(source, "absolute").toPath(), Paths.get("/data/data/other"));
        try { ExtraData.copy(source, destination); fail("An absolute link was copied"); }
        catch (java.io.IOException expected) { }
        Files.delete(new File(source, "absolute").toPath());
        Files.createSymbolicLink(new File(source, "up").toPath(), Paths.get("../outside"));
        try { ExtraData.copy(source, destination); fail("An escaping link was copied"); }
        catch (java.io.IOException expected) { }
        assertTrue(ExtraData.escapes(Paths.get("..")));
        assertTrue(ExtraData.escapes(Paths.get("a/../../b")));
        assertFalse(ExtraData.escapes(Paths.get("a/./b")));
    }

    @Test public void forgetRemovesNestedOutputWithoutFollowingLinks() throws Exception {
        File baseline = temporary();
        File extra = new File(baseline, "extra"), state = new File(baseline, "extra.state");
        new File(extra, "opt/google").mkdirs();
        Files.write(new File(extra, "opt/google/chrome").toPath(), new byte[]{1});
        Files.write(state.toPath(), COMMIT.getBytes(StandardCharsets.UTF_8));
        File outside = new File(baseline, "outside");
        assertTrue(outside.mkdir());
        Files.createSymbolicLink(new File(extra, "escape").toPath(), outside.toPath());
        ExtraData.forget(extra, state);
        assertFalse("The private output is gone", extra.exists());
        assertFalse("The marker is gone", state.exists());
        assertTrue("A link out of the tree is never followed", outside.isDirectory());
        ExtraData.forget(extra, state);
    }
}
