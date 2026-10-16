package org.matonos.linux.stub;

import static org.junit.Assert.*;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.Locale;

import org.junit.Test;

/** The ustar stream that carries an applied extra tree across the pipe. */
public final class TarTest {
    @Test public void roundTripsFilesDirectoriesAndLinks() throws Exception {
        Path source = Files.createTempDirectory("tar-source");
        Files.createDirectories(source.resolve("bin"));
        Files.createDirectories(source.resolve("empty"));
        Files.write(source.resolve("bin/apply_extra"), "#!/bin/sh\n".getBytes(StandardCharsets.UTF_8));
        assertTrue(source.resolve("bin/apply_extra").toFile().setExecutable(true, true));
        Files.write(source.resolve("data.bin"), new byte[]{1, 2, 3, 4, 5});
        Files.createSymbolicLink(source.resolve("link"), Path.of("data.bin"));

        ByteArrayOutputStream archive = new ByteArrayOutputStream();
        Tar.write(source.toFile(), archive);

        Path destination = Files.createTempDirectory("tar-destination").resolve("extra");
        Tar.extract(new ByteArrayInputStream(archive.toByteArray()), destination.toFile(), 1 << 20);

        assertArrayEquals("#!/bin/sh\n".getBytes(StandardCharsets.UTF_8),
                Files.readAllBytes(destination.resolve("bin/apply_extra")));
        assertTrue("An applied binary stays executable",
                Files.isExecutable(destination.resolve("bin/apply_extra")));
        assertTrue(Files.isDirectory(destination.resolve("empty")));
        assertArrayEquals(new byte[]{1, 2, 3, 4, 5}, Files.readAllBytes(destination.resolve("data.bin")));
        assertTrue(Files.isSymbolicLink(destination.resolve("link")));
        assertEquals("data.bin", Files.readSymbolicLink(destination.resolve("link")).toString());
    }

    @Test public void writeRejectsEscapingSymlink() throws Exception {
        Path source = Files.createTempDirectory("tar-link-source");
        Files.createSymbolicLink(source.resolve("escape"), Path.of("../outside"));
        try {
            Tar.write(source.toFile(), new ByteArrayOutputStream());
            fail("An escaping link was archived");
        } catch (java.io.IOException expected) { }
    }

    @Test public void extractRejectsPrivilegedMode() throws Exception {
        byte[] archive = archive(entry("run.sh", 04755, '0', "", "x".getBytes(StandardCharsets.UTF_8)));
        Path destination = Files.createTempDirectory("tar-setuid").resolve("extra");
        try {
            Tar.extract(new ByteArrayInputStream(archive), destination.toFile(), 1 << 20);
            fail("A setuid entry was extracted");
        } catch (java.io.IOException expected) { }
    }

    @Test public void extractRejectsEscapingName() throws Exception {
        byte[] archive = archive(entry("../escape", 0644, '0', "", "x".getBytes(StandardCharsets.UTF_8)));
        Path destination = Files.createTempDirectory("tar-name").resolve("extra");
        try {
            Tar.extract(new ByteArrayInputStream(archive), destination.toFile(), 1 << 20);
            fail("An escaping name was extracted");
        } catch (java.io.IOException expected) { }
    }

    @Test public void extractRejectsAbsoluteLink() throws Exception {
        byte[] archive = archive(entry("link", 0777, '2', "/etc/passwd", null));
        Path destination = Files.createTempDirectory("tar-absolute").resolve("extra");
        try {
            Tar.extract(new ByteArrayInputStream(archive), destination.toFile(), 1 << 20);
            fail("An absolute link was extracted");
        } catch (java.io.IOException expected) { }
    }

    @Test public void extractRejectsTruncation() throws Exception {
        byte[] full = archive(entry("data.bin", 0644, '0', "", "payload".getBytes(StandardCharsets.UTF_8)));
        byte[] truncated = Arrays.copyOf(full, 512 + 3);
        Path destination = Files.createTempDirectory("tar-truncated").resolve("extra");
        try {
            Tar.extract(new ByteArrayInputStream(truncated), destination.toFile(), 1 << 20);
            fail("A truncated archive was extracted");
        } catch (java.io.IOException expected) { }
    }

    @Test public void extractRejectsOversizedArchiveBeforeReadingPayload() throws Exception {
        byte[] header = entry("large.bin", 0644, '0', "", new byte[0]);
        // Declare a very large file while keeping the stream tiny. The bound
        // must be checked before extraction allocates or waits for its body.
        octal(header, 124, 12, 4096);
        Arrays.fill(header, 148, 156, (byte) ' ');
        int checksum = 0;
        for (byte value : header) checksum += value & 255;
        byte[] digits = String.format(Locale.ROOT, "%06o", checksum).getBytes(StandardCharsets.US_ASCII);
        System.arraycopy(digits, 0, header, 148, 6);
        header[154] = 0; header[155] = ' ';
        Path destination = Files.createTempDirectory("tar-size").resolve("extra");
        try {
            Tar.extract(new ByteArrayInputStream(archive(header)), destination.toFile(), 1024);
            fail("An oversized archive was accepted");
        } catch (java.io.IOException expected) { }
    }

    /** One complete archive: the entry, zero padding, then two zero blocks. */
    private static byte[] archive(byte[] entry) throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        out.write(entry, 0, entry.length);
        int padding = (512 - (entry.length % 512)) % 512;
        out.write(new byte[padding]);
        out.write(new byte[1024]);
        return out.toByteArray();
    }

    /** A ustar entry with a correct checksum, so only the tested rule fails. */
    private static byte[] entry(String name, int mode, char type, String link, byte[] content) throws Exception {
        byte[] header = new byte[512];
        put(header, 0, name);
        octal(header, 100, 8, mode);
        octal(header, 108, 8, 0);
        octal(header, 116, 8, 0);
        octal(header, 124, 12, content == null ? 0 : content.length);
        octal(header, 136, 12, 0);
        Arrays.fill(header, 148, 156, (byte) ' ');
        header[156] = (byte) type;
        put(header, 157, link);
        System.arraycopy("ustar".getBytes(StandardCharsets.US_ASCII), 0, header, 257, 5);
        header[263] = '0';
        header[264] = '0';
        int checksum = 0;
        for (byte value : header) checksum += value & 255;
        byte[] digits = String.format(Locale.ROOT, "%06o", checksum).getBytes(StandardCharsets.US_ASCII);
        System.arraycopy(digits, 0, header, 148, 6);
        header[154] = 0;
        header[155] = ' ';
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        out.write(header);
        if (content != null) out.write(content);
        return out.toByteArray();
    }

    private static void put(byte[] header, int offset, String value) {
        byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
        System.arraycopy(bytes, 0, header, offset, bytes.length);
    }

    private static void octal(byte[] header, int offset, int width, long value) {
        String text = Long.toOctalString(value);
        int digits = width - 1;
        int padding = digits - text.length();
        for (int i = 0; i < padding; i++) header[offset + i] = '0';
        for (int i = 0; i < text.length(); i++) header[offset + padding + i] = (byte) text.charAt(i);
        header[offset + digits] = 0;
    }
}
