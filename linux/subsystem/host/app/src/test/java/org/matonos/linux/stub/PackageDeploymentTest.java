package org.matonos.linux.stub;

import static org.junit.Assert.*;
import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.zip.*;
import org.junit.Test;

public final class PackageDeploymentTest {
    @Test public void GNOME51DoesNotRequireAnInstalledCompatExtension() throws Exception {
        assertFalse(PackageDeployment.appDeclaresCompatI386(appRecord("app/org.gnome.TextEditor/x86_64/stable",
                java.util.Collections.emptyList()),"51"));
    }
    @Test public void SteamLikeAppUsesCompatForItsOwnRuntimeBranch() throws Exception {
        org.matonos.linuxhost.stubgen.DeploymentRecord.Dependency compat=
                new org.matonos.linuxhost.stubgen.DeploymentRecord.Dependency(
                        "runtime/org.freedesktop.Platform.Compat.i386/x86_64/24.08","flathub",
                        "/usr/lib/i386-linux-gnu");
        org.matonos.linuxhost.stubgen.DeploymentRecord steam=appRecord(
                "app/com.valvesoftware.Steam/x86_64/stable",java.util.Collections.singletonList(compat));
        assertTrue(PackageDeployment.appDeclaresCompatI386(steam,"24.08"));
        assertFalse(PackageDeployment.appDeclaresCompatI386(steam,"25.08"));
    }
    private static org.matonos.linuxhost.stubgen.DeploymentRecord appRecord(String ref,
            java.util.List<org.matonos.linuxhost.stubgen.DeploymentRecord.Dependency> dependencies) throws Exception {
        byte[] object={7,3,1};
        String commit=hex(java.security.MessageDigest.getInstance("SHA-256").digest(object));
        return new org.matonos.linuxhost.stubgen.DeploymentRecord(ref,commit,"flathub",
                "[Application]\n".getBytes(StandardCharsets.UTF_8),new byte[]{1},object,dependencies);
    }
    @Test public void materializesBundledGlExtensionAsInstalledRuntime() throws Exception {
        materialize("26.08");
    }
    @Test public void materializesBundledGlExtensionForOtherRuntimeBranch() throws Exception {
        materialize("25.08");
    }
    @Test public void materializesBundledGl32Extension() throws Exception {
        Path scratch=Files.createTempDirectory("maton-gl32-extension");
        byte[] commitObject={9,8,7,6};
        String commit=hex(java.security.MessageDigest.getInstance("SHA-256").digest(commitObject));
        ByteBuffer objects=ByteBuffer.allocate(8+4+32+8+commitObject.length).order(ByteOrder.LITTLE_ENDIAN);
        objects.put("MTPKOBJ1".getBytes(StandardCharsets.US_ASCII)).putInt(1);
        objects.put(java.security.MessageDigest.getInstance("SHA-256").digest(commitObject));
        objects.putLong(commitObject.length).put(commitObject);
        Path apk=scratch.resolve("host.apk");
        try(ZipOutputStream zip=new ZipOutputStream(Files.newOutputStream(apk))) {
            put(zip,"assets/gl-maton-i386/ref.txt","runtime/org.freedesktop.Platform.GL32.maton/x86_64/26.08\n"+commit+"\n");
            zip.putNextEntry(new ZipEntry("assets/gl-maton-i386/objects.ostree"));zip.write(objects.array());zip.closeEntry();
            put(zip,"assets/gl-maton-i386/tree/lib/libEGL_maton.so","gl32");
        }
        Path system=scratch.resolve("system");Files.createDirectories(system);
        File files=PackageDeployment.prepareGl32Extension(system.toFile(),apk.toFile(),"25.08");
        Path version=system.resolve("runtime/org.freedesktop.Platform.GL32.maton/x86_64/25.08").resolve(commit);
        assertEquals(version.resolve("files").toFile(),files);
        assertEquals(PackageDeployment.gl32Metadata("25.08"),read(version.resolve("metadata")));
        assertArrayEquals(DeployRecord.extension("flathub",commit,"25.08"),Files.readAllBytes(version.resolve("deploy")));
        assertEquals("gl32",read(files.toPath().resolve("lib/libEGL_maton.so")));
        assertArrayEquals(commitObject,Files.readAllBytes(system.resolve("repo/objects").resolve(commit.substring(0,2)).resolve(commit.substring(2)+".commit")));
    }
    @Test public void i386ExtensionPointsAreAddedOnceAndLinkTheInterpreter() throws Exception {
        String original="[Runtime]\nname=org.freedesktop.Platform\n";
        String updated=PackageDeployment.withI386ExtensionPoints(original,"25.08");
        assertEquals(updated,PackageDeployment.withI386ExtensionPoints(updated,"25.08"));
        assertTrue(updated.contains("[Extension org.freedesktop.Platform.Compat.i386]"));
        assertTrue(updated.contains("directory=lib/i386-linux-gnu/GL"));
        Path root=Files.createTempDirectory("maton-i386-runtime");
        Path branch=root.resolve("runtime/org.freedesktop.Platform/x86_64/25.08");
        Path version=branch.resolve("a".repeat(64));Files.createDirectories(version.resolve("files"));
        Files.write(version.resolve("metadata"),original.getBytes(StandardCharsets.UTF_8));Files.createSymbolicLink(branch.resolve("active"),Path.of("a".repeat(64)));
        PackageDeployment.enableI386Runtime(root.toFile(),"25.08");
        assertEquals(updated,read(version.resolve("metadata")));
        assertEquals(Path.of("/usr/lib/i386-linux-gnu/ld-linux.so.2"),Files.readSymbolicLink(version.resolve("files/lib/ld-linux.so.2")));
    }
    /** Packaged gl-maton/build.py record for commit 6fbb4ff1… on branch 26.08. */
    @Test public void deployRecordMatchesBuildPy() {
        String golden="666c61746875620036666262346666313537656337323864613563373037346662373735316163306463326338646465346235386339383532613833306637356230653737323139000000000000000000000000000000006465706c6f792d76657273696f6e00000400000000690f0074696d657374616d7000000000000000000000000000000000740a000000000072756e74696d65006f72672e667265656465736b746f702e506c6174666f726d2f7838365f36342f32362e30380000730800000000000000657874656e73696f6e2d6f660000000072756e74696d652f6f72672e667265656465736b746f702e506c6174666f726d2f7838365f36342f32362e30380000730d173369b1490049000800";
        assertEquals(golden,hex(DeployRecord.extension("flathub",
                "6fbb4ff157ec728da5c7074fb7751ac0dc2c8dde4b58c9852a830f75b0e77219","26.08")));
    }
    private static void materialize(String branch) throws Exception {
        Path scratch=Files.createTempDirectory("maton-gl-extension");
        byte[] commitObject={1,2,3,4};
        String commit=hex(java.security.MessageDigest.getInstance("SHA-256").digest(commitObject));
        String ref="runtime/org.freedesktop.Platform.GL.maton/x86_64/"+branch;
        ByteBuffer objects=ByteBuffer.allocate(8+4+32+8+commitObject.length).order(ByteOrder.LITTLE_ENDIAN);
        objects.put("MTPKOBJ1".getBytes(StandardCharsets.US_ASCII)).putInt(1);
        objects.put(java.security.MessageDigest.getInstance("SHA-256").digest(commitObject));
        objects.putLong(commitObject.length).put(commitObject);
        Path apk=scratch.resolve("host.apk");
        try(ZipOutputStream zip=new ZipOutputStream(Files.newOutputStream(apk))) {
            // Bundled template is always built for 26.08.
            put(zip,"assets/gl-maton/ref.txt","runtime/org.freedesktop.Platform.GL.maton/x86_64/26.08\n"+commit+"\n");
            zip.putNextEntry(new ZipEntry("assets/gl-maton/objects.ostree"));zip.write(objects.array());zip.closeEntry();
            put(zip,"assets/gl-maton/tree/lib/libmaton_vulkan.so","vulkan");
        }
        Path system=scratch.resolve("system");Files.createDirectories(system);
        File files=PackageDeployment.prepareGlExtension(system.toFile(),apk.toFile(),branch);
        Path version=system.resolve(ref).resolve(commit);
        assertEquals(version.resolve("files").toFile(),files);
        assertTrue(Files.isDirectory(files.toPath()));
        assertEquals("vulkan",read(files.toPath().resolve("lib/libmaton_vulkan.so")));
        assertTrue(Files.isRegularFile(version.resolve("metadata")));
        assertArrayEquals(DeployRecord.extension("flathub",commit,branch),Files.readAllBytes(version.resolve("deploy")));
        assertTrue(new String(Files.readAllBytes(version.resolve("deploy")),StandardCharsets.UTF_8)
                .contains("runtime/org.freedesktop.Platform/x86_64/"+branch));
        assertEquals(PackageDeployment.metadata(branch),new String(Files.readAllBytes(version.resolve("metadata")),StandardCharsets.UTF_8));
        assertTrue(Files.isRegularFile(version.resolve(".ref")));
        assertTrue(Files.isSymbolicLink(system.resolve(ref).resolve("active")));
        assertTrue(Files.isSymbolicLink(system.resolve("runtime/org.freedesktop.Platform.GL.maton/current")));
        assertEquals(Path.of("x86_64",branch),Files.readSymbolicLink(system.resolve("runtime/org.freedesktop.Platform.GL.maton/current")));
        assertArrayEquals(commitObject,Files.readAllBytes(system.resolve("repo/objects").resolve(commit.substring(0,2)).resolve(commit.substring(2)+".commit")));
    }
    private static void put(ZipOutputStream zip,String name,String value)throws IOException {
        zip.putNextEntry(new ZipEntry(name));zip.write(value.getBytes(StandardCharsets.UTF_8));zip.closeEntry();
    }
    private static String read(Path path)throws IOException {
        return new String(Files.readAllBytes(path),StandardCharsets.UTF_8);
    }
    private static String hex(byte[] bytes) {
        StringBuilder value=new StringBuilder(bytes.length*2);
        for(byte b:bytes)value.append(String.format(java.util.Locale.ROOT,"%02x",b&0xff));
        return value.toString();
    }
}
