package org.matonos.linux.stub;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.Arrays;
import java.util.Comparator;

/**
 * POSIX ustar (type {@code ustar\0}, version {@code 00}) streaming of an
 * applied extra tree, plus a strict reader the subsystem app validates with.
 *
 * <p>No Android dependency, so both halves are unit-testable on the JVM. The
 * writer never emits setuid, setgid, sticky, hard-link, device or FIFO
 * entries; the reader rejects them (and any name that would escape the
 * destination) rather than trusting the producer. A vendor {@code apply_extra}
 * runs untrusted code under the app's own UID, so the output is untrusted
 * input to the subsystem app.
 */
public final class Tar {
    private static final int BLOCK = 512;
    private static final byte[] MAGIC = {'u', 's', 't', 'a', 'r', 0};

    private Tar() { }

    /** Streams {@code root}'s tree as a ustar archive. Symlinks are written,
     * never followed; only regular files and directories carry content. */
    public static void write(File root, OutputStream out) throws IOException {
        if (!root.isDirectory() || Files.isSymbolicLink(root.toPath()))
            throw new IOException("Missing extra output directory");
        writeDirectory(root, "", out);
        out.write(new byte[BLOCK * 2]);
        out.flush();
    }

    private static void writeDirectory(File directory, String prefix, OutputStream out) throws IOException {
        File[] children = directory.listFiles();
        if (children == null) throw new IOException("Cannot list " + directory);
        Arrays.sort(children, Comparator.comparing(File::getName));
        for (File child : children) {
            String name = child.getName();
            if (name.isEmpty() || name.equals(".") || name.equals("..") || name.indexOf('/') >= 0)
                throw new IOException("Unsafe extra entry");
            String path = prefix + name;
            Path source = child.toPath();
            if (Files.isSymbolicLink(source)) {
                Path target = Files.readSymbolicLink(source);
                if (target.isAbsolute() || escapes(target))
                    throw new IOException("Unsafe extra link " + path);
                entry(out, path, 0777, 0, '2', target.toString(), null);
            } else if (Files.isDirectory(source, LinkOption.NOFOLLOW_LINKS)) {
                entry(out, path + "/", 0755, 0, '5', "", null);
                writeDirectory(child, path + "/", out);
            } else if (Files.isRegularFile(source, LinkOption.NOFOLLOW_LINKS)) {
                long length = Files.size(source);
                int mode = Files.isExecutable(source) ? 0755 : 0644;
                entry(out, path, mode, length, '0', "", child);
            } else {
                throw new IOException("Unsupported extra entry " + path);
            }
        }
    }

    private static void entry(OutputStream out, String name, int mode, long size, char typeflag,
            String link, File content) throws IOException {
        if (mode < 0 || (mode & 07000) != 0) throw new IOException("Unsupported extra mode");
        byte[] header = new byte[BLOCK];
        String filename = name, prefix = "";
        byte[] raw = name.getBytes(StandardCharsets.UTF_8);
        if (raw.length > 100) {
            int slash = name.lastIndexOf('/');
            if (slash < 1) throw new IOException("Extra entry name is too long");
            prefix = name.substring(0, slash);
            filename = name.substring(slash + 1);
            if (prefix.getBytes(StandardCharsets.UTF_8).length > 155
                    || filename.getBytes(StandardCharsets.UTF_8).length > 100)
                throw new IOException("Extra entry name is too long");
        }
        put(header, 0, filename, 100);
        octal(header, 100, 8, mode);
        octal(header, 108, 8, 0);
        octal(header, 116, 8, 0);
        octal(header, 124, 12, size);
        octal(header, 136, 12, 0);
        Arrays.fill(header, 148, 156, (byte) ' ');
        header[156] = (byte) typeflag;
        put(header, 157, link, 100);
        System.arraycopy(MAGIC, 0, header, 257, MAGIC.length);
        header[263] = '0';
        header[264] = '0';
        put(header, 345, prefix, 155);
        int checksum = 0;
        for (byte value : header) checksum += value & 255;
        byte[] digits = String.format(java.util.Locale.ROOT, "%06o", checksum).getBytes(StandardCharsets.US_ASCII);
        System.arraycopy(digits, 0, header, 148, 6);
        header[154] = 0;
        header[155] = ' ';
        out.write(header);
        if (content != null) {
            try (InputStream in = Files.newInputStream(content.toPath())) {
                byte[] buffer = new byte[65536];
                long written = 0;
                int read;
                while ((read = in.read(buffer)) != -1) { out.write(buffer, 0, read); written += read; }
                if (written != size) throw new IOException("Extra entry changed while archiving");
            }
            long padding = (BLOCK - (size % BLOCK)) % BLOCK;
            if (padding > 0) out.write(new byte[(int) padding]);
        }
    }

    /**
     * Extracts a ustar archive into a fresh {@code destination}, rejecting
     * anything the producer must not send: absolute or escaping names, {@code ..}
     * components, unsupported types, setuid/setgid/sticky modes, escaping
     * relative symlinks, duplicate entries and totals beyond the limits.
     */
    public static void extract(InputStream in, File destination, long maxBytes) throws IOException {
        if (maxBytes < 0) throw new IOException("Invalid extra tree size limit");
        if (destination.exists()) throw new IOException("Extra output directory already exists");
        if (!destination.mkdirs()) throw new IOException("Cannot create extra output directory");
        byte[] header = new byte[BLOCK];
        long total = 0;
        long archiveBytes = 0;
        while (true) {
            if (!read(in, header)) throw new IOException("Truncated extra archive");
            if (archiveBytes > Long.MAX_VALUE - BLOCK) throw new IOException("Extra archive size overflows");
            archiveBytes += BLOCK;
            if (isZero(header)) {
                if (!read(in, header) || !isZero(header)) throw new IOException("Invalid extra archive trailer");
                return;
            }
            if (!Arrays.equals(MAGIC, Arrays.copyOfRange(header, 257, 263))
                    || header[263] != '0' || header[264] != '0')
                throw new IOException("Unsupported extra archive format");
            int checksum = 0;
            for (int i = 0; i < BLOCK; i++) checksum += (i >= 148 && i < 156 ? ' ' : header[i]) & 255;
            if (checksum != (int) octal(header, 148, 8)) throw new IOException("Corrupt extra archive header");
            String name = field(header, 0, 100);
            String prefix = field(header, 345, 155);
            if (!prefix.isEmpty()) name = prefix + "/" + name;
            int mode = (int) octal(header, 100, 8);
            long size = octal(header, 124, 12);
            char type = (char) (header[156] == 0 ? '0' : header[156]);
            String link = field(header, 157, 100);
            if (type == '5' && name.endsWith("/")) name = name.substring(0, name.length() - 1);
            validateName(name);
            if ((mode & 07000) != 0) throw new IOException("Extra entry " + name + " has a privileged mode");
            if (type != '0' && type != '5' && type != '2')
                throw new IOException("Unsupported extra entry type for " + name);
            if (size < 0 || size > ExtraProtocol.MAX_ENTRY_BYTES)
                throw new IOException("Extra entry " + name + " is too large");
            long payloadSize = type == '0' ? size : 0;
            long padded = payloadSize + ((BLOCK - (payloadSize % BLOCK)) % BLOCK);
            if (padded < size || archiveBytes > ExtraProtocol.MAX_OUTPUT_BYTES - BLOCK
                    || padded > ExtraProtocol.MAX_OUTPUT_BYTES - archiveBytes - BLOCK)
                throw new IOException("Extra archive is too large");
            archiveBytes += padded;
            if (type == '2') {
                if (link.isEmpty() || link.startsWith("/") || escapes(Path.of(link)))
                    throw new IOException("Extra link " + name + " escapes its tree");
            }
            String relative = name.endsWith("/") ? name.substring(0, name.length() - 1) : name;
            File target = new File(destination, relative);
            if (type == '5') {
                ensureDirectories(destination, relative);
                if (Files.exists(target.toPath(), LinkOption.NOFOLLOW_LINKS)) {
                    if (!Files.isDirectory(target.toPath(), LinkOption.NOFOLLOW_LINKS))
                        throw new IOException("Duplicate extra entry " + name);
                    // A child may have caused implicit parent creation before
                    // the directory's explicit ustar entry appeared.
                } else if (!target.mkdir()) throw new IOException("Cannot create extra directory " + name);
            } else if (type == '2') {
                String parent = relative.lastIndexOf('/') < 0 ? "" : relative.substring(0, relative.lastIndexOf('/'));
                ensureDirectories(destination, parent);
                if (Files.exists(target.toPath(), LinkOption.NOFOLLOW_LINKS))
                    throw new IOException("Duplicate extra entry " + name);
                Files.createSymbolicLink(target.toPath(), Path.of(link));
            } else {
                String parent = relative.lastIndexOf('/') < 0 ? "" : relative.substring(0, relative.lastIndexOf('/'));
                ensureDirectories(destination, parent);
                if (Files.exists(target.toPath(), LinkOption.NOFOLLOW_LINKS))
                    throw new IOException("Duplicate extra entry " + name);
                long remaining = size;
                if (total > maxBytes - size) throw new IOException("Extra tree is too large");
                total += size;
                try (OutputStream out = Files.newOutputStream(target.toPath(), StandardOpenOption.CREATE_NEW)) {
                    byte[] buffer = new byte[65536];
                    while (remaining > 0) {
                        int wanted = (int) Math.min(remaining, buffer.length);
                        int read = in.read(buffer, 0, wanted);
                        if (read < 0) throw new IOException("Truncated extra archive");
                        out.write(buffer, 0, read);
                        remaining -= read;
                    }
                }
                // Files are created non-executable; the archive's mode decides
                // the bit, so an app's applied binaries stay runnable.
                if ((mode & 0111) != 0 && !target.setExecutable(true, true))
                    throw new IOException("Cannot mark extra entry executable");
                skip(in, (BLOCK - (size % BLOCK)) % BLOCK);
            }
            if (type == '5' || type == '2') skip(in, size);
        }
    }

    private static void ensureDirectories(File destination, String relative) throws IOException {
        if (relative.isEmpty()) return;
        File current = destination;
        for (String part : relative.split("/", -1)) {
            if (part.isEmpty() || part.equals(".") || part.equals(".."))
                throw new IOException("Unsafe extra path");
            current = new File(current, part);
            Path path = current.toPath();
            if (Files.isSymbolicLink(path)) throw new IOException("Extra path crosses a symlink");
            if (Files.exists(path, LinkOption.NOFOLLOW_LINKS)) {
                if (!Files.isDirectory(path, LinkOption.NOFOLLOW_LINKS))
                    throw new IOException("Extra path is not a directory");
            } else if (!current.mkdir()) throw new IOException("Cannot create extra directory");
        }
    }

    private static void validateName(String name) throws IOException {
        if (name.isEmpty() || name.length() > 255 || name.startsWith("/"))
            throw new IOException("Unsafe extra entry name");
        for (String part : name.split("/", -1))
            if (part.isEmpty() || part.equals(".") || part.equals(".."))
                throw new IOException("Unsafe extra entry name");
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            if (c < 32 || c == 127) throw new IOException("Unsafe extra entry name");
        }
    }

    private static boolean escapes(Path target) {
        for (Path part : target) if (part.toString().equals("..")) return true;
        return false;
    }

    private static void put(byte[] header, int offset, String value, int width) throws IOException {
        byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
        if (bytes.length > width) throw new IOException("Extra header field is too long");
        System.arraycopy(bytes, 0, header, offset, bytes.length);
    }

    private static void octal(byte[] header, int offset, int width, long value) throws IOException {
        int digits = width - 1;
        String text = Long.toOctalString(value);
        if (text.length() > digits || value < 0) throw new IOException("Extra header number is too large");
        for (int i = 0; i < digits - text.length(); i++) header[offset + i] = '0';
        for (int i = 0; i < text.length(); i++) header[offset + digits - text.length() + i] = (byte) text.charAt(i);
        header[offset + digits] = 0;
    }

    private static long octal(byte[] header, int offset, int width) throws IOException {
        int end = offset + width;
        while (end > offset && (header[end - 1] == 0 || header[end - 1] == ' ')) end--;
        long value = 0;
        for (int i = offset; i < end; i++) {
            int digit = header[i] & 255;
            if (digit == 0 || digit == ' ') {
                for (int j = i; j < end; j++) if (header[j] != 0 && header[j] != ' ')
                    throw new IOException("Corrupt extra header number");
                break;
            }
            if (digit < '0' || digit > '7') throw new IOException("Corrupt extra header number");
            if (value > (Long.MAX_VALUE >>> 3)) throw new IOException("Extra header number overflows");
            value = (value << 3) | (digit - '0');
            if (value < 0) throw new IOException("Extra header number overflows");
        }
        return value;
    }

    private static String field(byte[] header, int offset, int width) {
        int end = offset;
        while (end < offset + width && header[end] != 0) end++;
        return new String(header, offset, end - offset, StandardCharsets.UTF_8);
    }

    private static boolean isZero(byte[] block) {
        for (byte value : block) if (value != 0) return false;
        return true;
    }

    private static boolean read(InputStream in, byte[] block) throws IOException {
        int offset = 0;
        while (offset < block.length) {
            int read = in.read(block, offset, block.length - offset);
            if (read < 0) {
                if (offset == 0) return false;
                throw new IOException("Truncated extra archive");
            }
            offset += read;
        }
        return true;
    }

    private static void skip(InputStream in, long count) throws IOException {
        long remaining = count;
        while (remaining > 0) {
            long skipped = in.skip(remaining);
            if (skipped <= 0) {
                if (in.read() < 0) throw new IOException("Truncated extra archive");
                skipped = 1;
            }
            remaining -= skipped;
        }
    }
}
