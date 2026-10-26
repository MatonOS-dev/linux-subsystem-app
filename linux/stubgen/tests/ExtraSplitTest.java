import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;
import org.matonos.linuxhost.stubgen.ObjectPacks;
import org.matonos.linuxhost.stubgen.StubGenerator;

/**
 * The extra split is a self-contained pack carrier: one payload pack plus the
 * embedded index/metadata, mounted at the package's extra directory. The
 * launcher and the extra tree are independent pack roots.
 */
public final class ExtraSplitTest {
    public static void main(String[] args) throws Exception {
        File root = new File(args[0], "extra-split");
        delete(root);
        root.mkdirs();

        // A single-pack tree: ObjectPacks.initial with an unbounded target must
        // not split a package's extra output, because one carrier APK carries
        // exactly one pack file.
        File tree = new File(root, "tree");
        new File(tree, "nested").mkdirs();
        Files.write(new File(tree, "download.bin").toPath(), new byte[5000]);
        Files.write(new File(tree, "nested/part").toPath(), "payload".getBytes(StandardCharsets.UTF_8));
        Files.createSymbolicLink(new File(tree, "link").toPath(), Paths.get("nested/part"));
        List<ObjectPacks.Entry> entries = ObjectPacks.fromTree(tree, "/");
        if (!entries.stream().anyMatch(e -> e.path.equals("/download.bin") && e.kind.equals("file")))
            throw new AssertionError("fromTree did not root a file at /");
        if (!entries.stream().anyMatch(e -> e.path.equals("/nested/part")))
            throw new AssertionError("fromTree omitted a nested file");
        File objects = new File(root, "objects");
        ObjectPacks.initial(entries, objects, Long.MAX_VALUE);
        long packs = 0;
        for (File file : objects.listFiles()) if (file.getName().matches("pack-[0-9a-f]{64}\\.zip")) packs++;
        if (packs != 1) throw new AssertionError("Extra output must be one pack, got " + packs);
        ObjectPacks.writeIndex(new File(objects, "objects.index"), entries);
        ObjectPacks.writeMetadata(new File(objects, "objects.ostree"), Collections.emptyMap());
        byte[] extraIndex = Files.readAllBytes(new File(objects, "objects.index").toPath());
        if (!ObjectPacks.hex(java.security.MessageDigest.getInstance("SHA-256").digest(extraIndex)).matches("[0-9a-f]{64}"))
            throw new AssertionError("Index is unreadable");
        String extraName = StubGenerator.extraSplitName(objects);
        if (!extraName.matches("extra_[0-9a-f]{64}")) throw new AssertionError("Extra split name: " + extraName);
        File unsigned = StubGenerator.generateExtraUnsigned(root, "org.example.Package", objects, 0, null, null, null);
        if (!StubGenerator.validExtraIdentity(unsigned, "org.example.Package", extraName))
            throw new AssertionError("Unsigned extra split identity did not validate");
        if (StubGenerator.validExtraIdentity(unsigned, "org.example.Other", extraName))
            throw new AssertionError("Unsigned extra split accepted the wrong package");
        // A package-built split carrying components (here a launcher alias) must
        // not be signed by the subsystem key.
        File withComponents = StubGenerator.generateExtraUnsigned(root, "org.example.Package", objects, 0,
                "Example", Collections.emptyList(), new byte[] {(byte) 0x89, 'P', 'N', 'G'});
        String componentsName = StubGenerator.extraSplitName(objects, "Example", Collections.emptyList(),
                new byte[] {(byte) 0x89, 'P', 'N', 'G'});
        if (StubGenerator.validExtraIdentity(withComponents, "org.example.Package", componentsName))
            throw new AssertionError("Extra split with components or resources was accepted");

        // fromTree must refuse an escaping symlink: the applied output is
        // untrusted input to the subsystem app.
        File unsafe = new File(root, "unsafe");
        unsafe.mkdirs();
        Files.createSymbolicLink(new File(unsafe, "up").toPath(), Paths.get("../escape"));
        try { ObjectPacks.fromTree(unsafe, "/"); throw new AssertionError("Escaping link accepted"); }
        catch (IOException expected) { }

        // The self-contained extra carrier embeds the same index and metadata.
        Map<String, byte[]> extraEntries = new LinkedHashMap<>();
        extraEntries.put("AndroidManifest.xml", new byte[]{3, 0, 8, 0});
        extraEntries.put("assets/matonos/objects.index", extraIndex);
        extraEntries.put("assets/matonos/objects.ostree",
                Files.readAllBytes(new File(objects, "objects.ostree").toPath()));
        File extraApk = new File(root, extraName + ".apk");
        zip(extraApk, extraEntries);

        System.out.println("PASS extra split is self-contained and its EROFS source is independent of package payload carriers");

        // The launcher source is a Flatpak .desktop export: only the Name, the
        // Icon name and the MIME associations are read, and localized keys are
        // ignored.
        StubGenerator.DesktopEntry desktop = StubGenerator.parseDesktop(
                "[Desktop Entry]\nType=Application\nName=Editor\nName[de]=Editor-de\nIcon=org.example.Editor\n"
                        + "MimeType=text/plain;image/png;bogus;\nComment=x\n[Desktop Action X]\nName=Ignored\n");
        if (!desktop.name.equals("Editor")) throw new AssertionError(desktop.name);
        if (!desktop.iconName.equals("org.example.Editor")) throw new AssertionError(desktop.iconName);
        if (!desktop.mimeTypes.equals(Arrays.asList("text/plain", "image/png"))) throw new AssertionError(desktop.mimeTypes.toString());
        try { StubGenerator.parseDesktop("Name=NoGroup\n"); throw new AssertionError("A group-less desktop file was accepted"); }
        catch (IOException expected) { }
    }

    private static void zip(File file, Map<String, byte[]> entries) throws Exception {
        try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(file))) {
            for (Map.Entry<String, byte[]> entry : entries.entrySet()) {
                ZipEntry row = new ZipEntry(entry.getKey());
                row.setMethod(ZipEntry.STORED);
                row.setSize(entry.getValue().length);
                row.setCompressedSize(entry.getValue().length);
                CRC32 crc = new CRC32(); crc.update(entry.getValue());
                row.setCrc(crc.getValue());
                zip.putNextEntry(row); zip.write(entry.getValue()); zip.closeEntry();
            }
        }
    }

    private static void delete(File tree) {
        if (tree.isDirectory()) {
            File[] children = tree.listFiles();
            if (children != null) for (File child : children) delete(child);
        }
        //noinspection ResultOfMethodCallIgnored
        tree.delete();
    }
}
