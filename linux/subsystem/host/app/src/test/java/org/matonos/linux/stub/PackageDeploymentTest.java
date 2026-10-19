package org.matonos.linux.stub;

import static org.junit.Assert.*;
import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.zip.*;
import org.junit.Test;

public final class PackageDeploymentTest {
    @Test public void materializesBundledGlExtensionAsInstalledRuntime() throws Exception {
        Path scratch=Files.createTempDirectory("maton-gl-extension");
        byte[] commitObject={1,2,3,4};
        String branch="26.08",commit=hex(java.security.MessageDigest.getInstance("SHA-256").digest(commitObject));
        String ref="runtime/org.freedesktop.Platform.GL.maton/x86_64/"+branch;
        ByteBuffer objects=ByteBuffer.allocate(8+4+32+8+commitObject.length).order(ByteOrder.LITTLE_ENDIAN);
        objects.put("MTPKOBJ1".getBytes(StandardCharsets.US_ASCII)).putInt(1);
        objects.put(java.security.MessageDigest.getInstance("SHA-256").digest(commitObject));
        objects.putLong(commitObject.length).put(commitObject);
        Path apk=scratch.resolve("host.apk");
        try(ZipOutputStream zip=new ZipOutputStream(Files.newOutputStream(apk))) {
            put(zip,"assets/gl-maton/ref.txt",ref+"\n"+commit+"\n");
            put(zip,"assets/gl-maton/metadata","[Runtime]\nname=org.freedesktop.Platform.GL.maton\n\n[ExtensionOf]\nref=runtime/org.freedesktop.Platform/x86_64/26.08\n");
            zip.putNextEntry(new ZipEntry("assets/gl-maton/objects.ostree"));zip.write(objects.array());zip.closeEntry();
        }
        Path system=scratch.resolve("system");Files.createDirectories(system);
        File files=PackageDeployment.prepareGlExtension(system.toFile(),apk.toFile(),branch);
        Path version=system.resolve(ref).resolve(commit);
        assertEquals(version.resolve("files").toFile(),files);
        assertTrue(Files.isDirectory(files.toPath()));
        assertTrue(Files.isRegularFile(version.resolve("metadata")));
        assertEquals("[Runtime]\nname=org.freedesktop.Platform.GL.maton\n\n[ExtensionOf]\nref=runtime/org.freedesktop.Platform/x86_64/26.08\n",
                new String(Files.readAllBytes(version.resolve("metadata")),StandardCharsets.UTF_8));
        assertTrue(Files.isRegularFile(version.resolve(".ref")));
        assertTrue(Files.isSymbolicLink(system.resolve(ref).resolve("active")));
        assertTrue(Files.isSymbolicLink(system.resolve("runtime/org.freedesktop.Platform.GL.maton/current")));
        assertEquals(Path.of("x86_64",branch),Files.readSymbolicLink(system.resolve("runtime/org.freedesktop.Platform.GL.maton/current")));
        assertArrayEquals(commitObject,Files.readAllBytes(system.resolve("repo/objects").resolve(commit.substring(0,2)).resolve(commit.substring(2)+".commit")));
    }
    private static void put(ZipOutputStream zip,String name,String value)throws IOException {
        zip.putNextEntry(new ZipEntry(name));zip.write(value.getBytes(StandardCharsets.UTF_8));zip.closeEntry();
    }
    private static String hex(byte[] bytes) {
        StringBuilder value=new StringBuilder(bytes.length*2);
        for(byte b:bytes)value.append(String.format(java.util.Locale.ROOT,"%02x",b&0xff));
        return value.toString();
    }
}
