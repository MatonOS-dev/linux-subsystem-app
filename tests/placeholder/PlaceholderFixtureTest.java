package org.matonos.linux.stub;

import org.json.JSONObject;
import org.matonos.linuxhost.stubgen.ErofsImage;
import org.matonos.linuxhost.stubgen.ObjectPacks;
import org.matonos.linuxhost.stubgen.PlaceholderFiles;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;

/**
 * Reads a staging tree produced by the real compatibility proxy and checks that
 * the Java-side resolution sees exactly what a copy-mode checkout contains.
 *
 * <p>Run by {@code tests/placeholder/run.sh}; arguments are
 * {@code installation copyCheckout remote appRef commit workspace}.
 * {@code installation} is the marked store root, so it holds
 * {@code .v3-inodes/}, {@code repo/} and {@code app/<id>/<arch>/<branch>/<commit>/}.
 */
public final class PlaceholderFixtureTest {
    private static int checks;

    private static void check(boolean ok, String message) {
        checks++;
        if (!ok) throw new AssertionError(message);
    }

    private static void equals(Object expected, Object actual, String message) {
        check(expected == null ? actual == null : expected.equals(actual),
                message + " (expected " + expected + ", got " + actual + ")");
    }

    public static void main(String[] args) throws Exception {
        File installation = new File(args[0]);
        File copyCheckout = new File(args[1]);
        String remote = args[2];
        String appRef = args[3];
        String commit = args[4];
        File workspace = new File(args[5]);
        Files.createDirectories(workspace.toPath());

        JSONObject manifest = DeployManifest.build(installation, appRef, remote, "fixture-op");
        equals(appRef, manifest.getString("appRef"), "manifest app ref");
        equals(remote, manifest.getString("remote"), "manifest remote");
        equals(commit, manifest.getString("appCommit"), "manifest commit");

        String stagedMetadata = DeployManifest.stagedMetadata(installation, manifest);
        byte[] copyMetadata = Files.readAllBytes(new File(copyCheckout, "metadata").toPath());
        equals(new String(copyMetadata, StandardCharsets.UTF_8), stagedMetadata,
                "staged metadata equals copy-mode metadata");

        File deployment = DeployManifest.stagedDeployment(installation, manifest);
        File deploy = new File(deployment, "deploy");
        File metadata = new File(deployment, "metadata");
        check(PlaceholderFiles.isPlaceholder(deploy), "deploy is an indexed placeholder");
        check(PlaceholderFiles.isPlaceholder(metadata), "metadata is an indexed placeholder");

        check(Arrays.equals(copyMetadata, PlaceholderFiles.readAllBytes(metadata)),
                "helper metadata bytes equal copy-mode");
        equals((long) copyMetadata.length, PlaceholderFiles.size(metadata), "helper metadata size");

        ByteArrayOutputStream copy = new ByteArrayOutputStream();
        equals((long) copyMetadata.length, PlaceholderFiles.copy(metadata, copy), "helper copy count");
        check(Arrays.equals(copyMetadata, copy.toByteArray()), "helper copy bytes");

        try (FileChannel channel = PlaceholderFiles.channel(metadata)) {
            ByteBuffer buffer = ByteBuffer.allocate(copyMetadata.length);
            while (buffer.hasRemaining()) channel.read(buffer);
            check(Arrays.equals(copyMetadata, buffer.array()), "helper channel bytes");
        }
        equals(2L, PlaceholderFiles.linkCount(metadata),
                "metadata persistent link count (seed + deployment)");

        // The commit object is read through the helper and must hash to the
        // commit id both the deploy metadata and the directory name carry.
        File commitObject = new File(installation,
                "repo/objects/" + commit.substring(0, 2) + "/" + commit.substring(2) + ".commit");
        equals(commit, sha256(PlaceholderFiles.readAllBytes(commitObject)),
                "commit object hashes to its content address");

        // Whole-tree content/size/checksum parity with the copy-mode checkout.
        File stagedFiles = new File(deployment, "files");
        compareTrees(stagedFiles, copyCheckout);
        check(hasPlaceholderFile(stagedFiles), "staged tree exposes at least one file placeholder");

        // Extra-data image construction preserves deployed modes as well as
        // content. Proxy hardlink markers are 01600 placeholders, so the mode
        // must come from the indexed backing inode just like the file bytes.
        List<ObjectPacks.Entry> stagedImage = ObjectPacks.fromTreeWithModes(stagedFiles, "/app/");
        List<ObjectPacks.Entry> copiedImage = ObjectPacks.fromTreeWithModes(copyCheckout, "/app/");
        equals(copiedImage.size(), stagedImage.size(), "image entry count");
        boolean resolvedPlaceholder = false;
        for (int i = 0; i < stagedImage.size(); i++) {
            ObjectPacks.Entry actual = stagedImage.get(i), expected = copiedImage.get(i);
            equals(expected.path, actual.path, "image path");
            equals(expected.kind, actual.kind, "image kind " + actual.path);
            equals(expected.checksum, actual.checksum, "image content " + actual.path);
            equals(expected.mode, actual.mode, "image mode " + actual.path);
            if (actual.kind.equals("file") && PlaceholderFiles.isPlaceholder(
                    new File(stagedFiles, actual.path.substring("/app/".length())))) {
                check(actual.mode != 01600, "image mode must not come from the 01600 marker");
                check(actual.source.equals(PlaceholderFiles.resolve(
                        new File(stagedFiles, actual.path.substring("/app/".length())))),
                        "image source resolves to proxy backing inode");
                resolvedPlaceholder = true;
            }
        }
        check(resolvedPlaceholder, "image fixture contains a resolved hardlink placeholder");

        File fakeMkfs = new File(workspace, "fake-mkfs-erofs");
        Files.writeString(fakeMkfs.toPath(), "#!/bin/sh\ncat \"${11}\" > \"${10}\"\n");
        check(fakeMkfs.setExecutable(true), "Cannot make fake mkfs executable");
        File imageOut = new File(workspace, "image");
        ErofsImage.build(stagedImage, imageOut, fakeMkfs, 0);
        File extractedImage = new File(workspace, "image-extracted");
        Files.createDirectories(extractedImage.toPath());
        Process extract = new ProcessBuilder("tar", "-xf", new File(imageOut, "deployment.erofs").toString(),
                "-C", extractedImage.toString()).inheritIO().start();
        if (extract.waitFor() != 0) throw new IOException("Cannot extract image fixture tar");
        for (ObjectPacks.Entry entry : copiedImage) {
            if (entry.path.equals("/app")) continue;
            File extracted = new File(extractedImage, "app/" + entry.path.substring("/app/".length()));
            equals(entry.mode, ((Number) Files.getAttribute(extracted.toPath(), "unix:mode",
                    java.nio.file.LinkOption.NOFOLLOW_LINKS)).intValue() & 07777,
                    "built image mode " + entry.path);
        }

        // The deterministic pack writer must consume the backing content, so
        // its entry identities, index and packs are byte-identical.
        List<ObjectPacks.Entry> staged = ObjectPacks.fromTree(stagedFiles, "/files/");
        List<ObjectPacks.Entry> copied = ObjectPacks.fromTree(copyCheckout, "/files/");
        equals(copied.size(), staged.size(), "entry count");
        for (int i = 0; i < staged.size(); i++) {
            ObjectPacks.Entry a = staged.get(i), b = copied.get(i);
            equals(b.path, a.path, "entry path");
            equals(b.checksum, a.checksum, "entry checksum " + a.path);
            equals(b.kind, a.kind, "entry kind " + a.path);
            equals(b.mode, a.mode, "entry mode " + a.path);
            equals(b.target, a.target, "entry target " + a.path);
        }

        File stagedOut = new File(workspace, "staged-packs");
        File copiedOut = new File(workspace, "copied-packs");
        ObjectPacks.initial(staged, stagedOut, ObjectPacks.DEFAULT_PACK_SIZE);
        ObjectPacks.initial(copied, copiedOut, ObjectPacks.DEFAULT_PACK_SIZE);
        ObjectPacks.writeIndex(new File(stagedOut, "objects.index"), staged);
        ObjectPacks.writeIndex(new File(copiedOut, "objects.index"), copied);
        compareDirectories(stagedOut, copiedOut);

        System.out.println("PASS PlaceholderFixtureTest (" + checks + " checks)");
    }

    /* ---- content/size/checksum parity -------------------------------------- */

    private static void compareTrees(File staged, File copied) throws IOException {
        Map<String, String> stagedEntries = walk(staged);
        Map<String, String> copiedEntries = walk(copied);
        equals(copiedEntries.keySet(), stagedEntries.keySet(), "tree paths");
        for (Map.Entry<String, String> entry : copiedEntries.entrySet())
            equals(entry.getValue(), stagedEntries.get(entry.getKey()), "tree entry " + entry.getKey());
    }

    /** {@code relative path -> "size:sha256"} for a whole tree. */
    private static Map<String, String> walk(File root) throws IOException {
        Map<String, String> result = new TreeMap<>();
        walkInto(root, "", result);
        return result;
    }

    private static void walkInto(File directory, String prefix, Map<String, String> result) throws IOException {
        File[] children = directory.listFiles();
        if (children == null) throw new IOException("Cannot list " + directory);
        Arrays.sort(children, (a, b) -> a.getName().compareTo(b.getName()));
        for (File child : children) {
            if (child.getName().equals(".v3-inodes")) continue;
            String relative = prefix + child.getName();
            if (Files.isDirectory(child.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS)) {
                walkInto(child, relative + "/", result);
            } else if (Files.isRegularFile(child.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS)) {
                byte[] content = PlaceholderFiles.readAllBytes(child);
                result.put(relative, PlaceholderFiles.size(child) + ":" + sha256(content));
            } else if (Files.isSymbolicLink(child.toPath())) {
                result.put(relative, "link:" + Files.readSymbolicLink(child.toPath()));
            }
        }
    }

    private static boolean hasPlaceholderFile(File root) throws IOException {
        List<PlaceholderFiles.Resolved> files = PlaceholderFiles.walk(root);
        for (PlaceholderFiles.Resolved resolved : files)
            if (PlaceholderFiles.isPlaceholder(resolved.path.toFile())) return true;
        return false;
    }

    /* ---- byte-identical pack directories ------------------------------------ */

    private static void compareDirectories(File expected, File actual) throws IOException {
        String[] wanted = expected.list();
        String[] seen = actual.list();
        if (wanted == null || seen == null) throw new IOException("Missing pack directory");
        Arrays.sort(wanted);
        Arrays.sort(seen);
        check(Arrays.equals(wanted, seen), "pack directory contents " + Arrays.toString(wanted)
                + " vs " + Arrays.toString(seen));
        for (String name : wanted) {
            byte[] a = Files.readAllBytes(new File(expected, name).toPath());
            byte[] b = Files.readAllBytes(new File(actual, name).toPath());
            check(Arrays.equals(a, b), "pack file " + name + " is not byte-identical");
        }
    }

    private static String sha256(byte[] data) {
        StringBuilder out = new StringBuilder();
        try {
            for (byte b : MessageDigest.getInstance("SHA-256").digest(data))
                out.append(String.format(Locale.ROOT, "%02x", b & 255));
        } catch (java.security.NoSuchAlgorithmException unavailable) {
            throw new AssertionError(unavailable);
        }
        return out.toString();
    }

    private PlaceholderFixtureTest() { }
}
