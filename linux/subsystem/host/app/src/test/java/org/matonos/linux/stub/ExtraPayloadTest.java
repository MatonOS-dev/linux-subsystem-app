package org.matonos.linux.stub;

import static org.junit.Assert.*;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import org.junit.Test;
import org.matonos.linuxhost.stubgen.ObjectPacks;

/** Copying a package's raw extra downloads out of its installed packs. */
public final class ExtraPayloadTest {
    @Test public void materializesIndexedRawDownloads() throws Exception {
        Path work = Files.createTempDirectory("extra-payload");
        Path tree = work.resolve("tree");
        Files.createDirectories(tree.resolve("bin"));
        Files.createDirectories(tree.resolve("extra/sub"));
        Files.write(tree.resolve("bin/apply_extra"), "#!/bin/sh\n".getBytes(StandardCharsets.UTF_8));
        byte[] payload = "raw-extra-bytes".getBytes(StandardCharsets.UTF_8);
        Files.write(tree.resolve("extra/sub/payload.bin"), payload);
        Files.createSymbolicLink(tree.resolve("extra/link"), Path.of("sub/payload.bin"));

        List<ObjectPacks.Entry> entries = ObjectPacks.fromTree(tree.toFile(), "/");
        File packs = Files.createDirectories(work.resolve("packs")).toFile();
        ObjectPacks.initial(entries, packs, Long.MAX_VALUE);
        File index = work.resolve("objects.index").toFile();
        ObjectPacks.writeIndex(index, entries);
        byte[] bytes = Files.readAllBytes(index.toPath());

        assertTrue("The tree has raw extra data", ExtraPayload.hasExtra(bytes));
        assertTrue("The tree has apply_extra", ExtraPayload.hasFile(bytes, ExtraPayload.APPLY_EXTRA));

        Map<String, String> carriers = new HashMap<>();
        String pack = null;
        for (ObjectPacks.Entry entry : entries)
            if (entry.kind.equals("file")) { pack = entry.pack; break; }
        assertNotNull(pack);
        carriers.put(pack, new File(packs, pack).getAbsolutePath());

        File destination = work.resolve("output").toFile();
        assertTrue(ExtraPayload.materialize(bytes, carriers, destination));
        assertArrayEquals(payload, Files.readAllBytes(new File(destination, "sub/payload.bin").toPath()));
        assertEquals("sub/payload.bin",
                Files.readSymbolicLink(new File(destination, "link").toPath()).toString());
        assertFalse("Non-extra entries stay in the pack", new File(destination, "bin").exists());
    }

    @Test public void reportsNoExtraData() throws Exception {
        Path work = Files.createTempDirectory("extra-none");
        Path tree = work.resolve("tree");
        Files.createDirectories(tree.resolve("bin"));
        Files.write(tree.resolve("bin/apply_extra"), "x".getBytes(StandardCharsets.UTF_8));
        List<ObjectPacks.Entry> entries = ObjectPacks.fromTree(tree.toFile(), "/");
        File packs = Files.createDirectories(work.resolve("packs")).toFile();
        ObjectPacks.initial(entries, packs, Long.MAX_VALUE);
        File index = work.resolve("objects.index").toFile();
        ObjectPacks.writeIndex(index, entries);

        assertFalse(ExtraPayload.hasExtra(Files.readAllBytes(index.toPath())));
        assertFalse(ExtraPayload.materialize(Files.readAllBytes(index.toPath()),
                new HashMap<>(), work.resolve("output").toFile()));
        assertFalse(new File(work.resolve("output").toFile(), "bin").exists());
    }

    @Test public void rejectsAnEscapingLinkInTheIndex() throws Exception {
        ObjectPacks.Entry link = new ObjectPacks.Entry("/extra/link", "0".repeat(64), "symlink", 0120777);
        link.target = "../../etc/passwd";
        File index = File.createTempFile("extra-index-", ".bin");
        ObjectPacks.writeIndex(index, List.of(link));
        try {
            ExtraPayload.materialize(Files.readAllBytes(index.toPath()),
                    new HashMap<>(), Files.createTempDirectory("extra-out-").toFile());
            fail("An escaping link was materialized");
        } catch (java.io.IOException expected) { }
    }
}
