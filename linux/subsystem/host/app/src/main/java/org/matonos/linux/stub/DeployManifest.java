package org.matonos.linux.stub;

import org.json.JSONArray;
import org.json.JSONObject;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

/**
 * Reads Flatpak's authenticated {@code deploy} metadata for a staged
 * installation and builds the exact completion manifest linuxd validates.
 *
 * <p>The manifest is only a hint: linuxd re-derives the app ID, remote,
 * commits and extension relations from signed OSTree metadata, so a wrong
 * value here fails verification rather than being trusted. Every value still
 * comes from Flatpak's own {@code deploy} GVariant (type
 * {@code (ssasta{sv})}) rather than a truncated CLI listing.
 */
final class DeployManifest {
    private static final String APP = "app";
    private static final String RUNTIME = "runtime";

    private static final class Deployment {
        final String ref;
        final String commit;
        final List<String> subpaths;
        Deployment(String ref, String commit, List<String> subpaths) {
            this.ref = ref; this.commit = commit; this.subpaths = subpaths;
        }
    }

    /**
     * Builds the verified-schema manifest for the authorized app ref. Fails if
     * the app deployment is missing, if an unexpected app deployment exists, or
     * if any deployment directory disagrees with its own signed metadata.
     */
    static JSONObject build(File installation, String appRef, String remote, String operationId) throws Exception {
        if (!validRef(appRef) || !appRef.startsWith(APP + "/"))
            throw new IOException("Invalid authorized application reference");
        List<Deployment> deployments = new ArrayList<>();
        scan(new File(installation, APP), APP, appRef, deployments);
        scan(new File(installation, RUNTIME), RUNTIME, appRef, deployments);
        String appCommit = null;
        for (Deployment deployment : deployments)
            if (deployment.ref.equals(appRef)) appCommit = deployment.commit;
        if (appCommit == null) throw new IOException("The authorized application was not deployed");
        JSONArray list = new JSONArray();
        for (Deployment deployment : deployments) {
            JSONArray subpaths = new JSONArray();
            for (String subpath : deployment.subpaths) subpaths.put(subpath);
            list.put(new JSONObject().put("ref", deployment.ref)
                    .put("commit", deployment.commit).put("subpaths", subpaths));
        }
        return new JSONObject().put("version", 1).put("operationId", operationId)
                .put("appRef", appRef).put("appCommit", appCommit).put("remote", remote)
                .put("deployments", list);
    }

    /**
     * Enumerates {@code <kind>/<id>/<arch>/<branch>/<commit[-subpath...]>}.
     * Symlinked bookkeeping entries such as {@code current} and {@code active}
     * are never followed, so one deployment is never counted twice.
     */
    private static void scan(File kindRoot, String kind, String appRef,
            List<Deployment> deployments) throws Exception {
        File[] ids = entries(kindRoot);
        if (ids == null) return;
        for (File id : ids) {
            if (!id.isDirectory() || symlink(id)) continue;
            File[] arches = entries(id);
            if (arches == null) continue;
            for (File arch : arches) {
                if (!arch.isDirectory() || symlink(arch)) continue;
                File[] branches = entries(arch);
                if (branches == null) continue;
                for (File branch : branches) {
                    if (!branch.isDirectory() || symlink(branch)) continue;
                    File[] versions = entries(branch);
                    if (versions == null) continue;
                    for (File version : versions) {
                        if (!version.isDirectory() || symlink(version)) continue;
                        File deploy = new File(version, "deploy");
                        if (!deploy.isFile() || symlink(deploy)) continue;
                        String ref = kind + "/" + id.getName() + "/" + arch.getName() + "/" + branch.getName();
                        if (!validRef(ref)) throw new IOException("Invalid staged deployment ref");
                        if (APP.equals(kind) && !ref.equals(appRef))
                            throw new IOException("Unexpected staged application deployment");
                        deployments.add(read(version, ref));
                    }
                }
            }
        }
    }

    private static Deployment read(File version, String ref) throws Exception {
        byte[] data;
        try {
            data = Files.readAllBytes(new File(version, "deploy").toPath());
        } catch (Exception error) {
            throw new IOException("Cannot read staged deployment metadata", error);
        }
        Metadata metadata;
        try {
            metadata = parse(data);
        } catch (Exception error) {
            throw new IOException("Malformed staged deployment metadata", error);
        }
        if (metadata.origin == null || !metadata.commit.matches("[0-9a-f]{64}"))
            throw new IOException("Staged deployment has no valid commit");
        StringBuilder expected = new StringBuilder(metadata.commit);
        for (String subpath : metadata.subpaths) {
            if (!validSubpath(subpath)) throw new IOException("Invalid staged deployment subpath");
            expected.append('-');
            for (int i = 0; i < subpath.length(); i++)
                if (subpath.charAt(i) != '/') expected.append(subpath.charAt(i));
        }
        if (!version.getName().equals(expected.toString()))
            throw new IOException("Staged deployment directory disagrees with its metadata");
        return new Deployment(ref, metadata.commit, metadata.subpaths);
    }

    private static final class Metadata {
        final String origin;
        final String commit;
        final List<String> subpaths;
        Metadata(String origin, String commit, List<String> subpaths) {
            this.origin = origin; this.commit = commit; this.subpaths = subpaths;
        }
    }

    /* ---- minimal (ssasta{sv}) reader; only origin, commit and subpaths ---- */

    /**
     * Flatpak's {@code deploy} file is {@code (ssasta{sv})}: origin, commit,
     * subpath array, a deploy version (uint64) and the metadata dictionary. The
     * subpath array is the only variable-size member this reader needs, and it
     * is followed by alignment padding, the 8-byte version and the dictionary.
     */
    private static Metadata parse(byte[] data) throws IOException {
        int length = data.length;
        if (length < 4) throw new IOException("deploy variant too small");
        int originEnd = cstringEnd(data, 0);
        String origin = cstring(data, 0, originEnd);
        int commitEnd = cstringEnd(data, originEnd);
        String commit = cstring(data, originEnd, commitEnd);
        int subpathsEnd = subpathsEnd(data, commitEnd);
        List<String> subpaths = stringArray(data, commitEnd, subpathsEnd);
        return new Metadata(origin, commit, subpaths);
    }

    /** Index one past the NUL that terminates the string starting at {@code from}. */
    private static int cstringEnd(byte[] data, long from) throws IOException {
        if (from < 0 || from >= data.length) throw new IOException("deploy string out of range");
        for (int i = (int) from; i < data.length; i++) if (data[i] == 0) return i + 1;
        throw new IOException("deploy string is not NUL terminated");
    }

    /**
     * Finds where the subpath array ends. It is padded to 8 and followed by the
     * fixed 8-byte deploy version, so the first offset whose parse is valid and
     * whose padding to the next 8-byte boundary is all zero is the array end.
     */
    private static int subpathsEnd(byte[] data, int from) throws IOException {
        for (int size = 0; from + size + 8 <= data.length; size++) {
            try { stringArray(data, from, from + size); }
            catch (IOException malformed) { continue; }
            int aligned = (from + size + 7) & ~7;
            if (aligned + 8 > data.length) continue;
            boolean zeros = true;
            for (int i = from + size; i < aligned; i++) if (data[i] != 0) { zeros = false; break; }
            if (zeros) return from + size;
        }
        throw new IOException("deploy subpath array is malformed");
    }

    private static String cstring(byte[] data, long begin, long end) throws IOException {
        if (begin < 0 || end > data.length || end <= begin) throw new IOException("deploy string out of range");
        int from = (int) begin, to = (int) (end - 1);
        if (data[to] != 0) throw new IOException("deploy string is not NUL terminated");
        for (int i = from; i < to; i++)
            if (data[i] == 0) throw new IOException("deploy string has an embedded NUL");
        return new String(data, from, to - from, StandardCharsets.UTF_8);
    }

    /**
     * Parses a GVariant {@code as}. Elements are packed NUL-terminated strings
     * followed by a table whose entries are each element's end offset; the last
     * entry therefore equals the table start, and the width is the smallest
     * that holds every offset.
     */
    private static List<String> stringArray(byte[] data, long begin, long end) throws IOException {
        if (begin < 0 || end > data.length || end < begin) throw new IOException("deploy array out of range");
        int from = (int) begin, size = (int) (end - begin);
        List<String> values = new ArrayList<>();
        if (size == 0) return values;
        for (int width : new int[] {1, 2, 4, 8}) {
            if (size < width) continue;
            for (int count = 1; count * width < size; count++) {
                int table = size - count * width;
                if (table < 1 || data[from + table - 1] != 0) continue;
                int[] ends = new int[count];
                boolean ok = true;
                for (int i = 0; i < count && ok; i++) {
                    long value = little(data, from + table + i * width, width);
                    if (value <= 0 || value > table) ok = false;
                    else ends[i] = (int) value;
                }
                if (!ok || ends[count - 1] != table) continue;
                for (int i = 1; i < count && ok; i++) if (ends[i] <= ends[i - 1]) ok = false;
                for (int i = 0; i < count && ok; i++) {
                    int start = from + (i == 0 ? 0 : ends[i - 1]);
                    int stop = from + ends[i];
                    if (stop <= start || data[stop - 1] != 0) { ok = false; break; }
                    for (int j = start; j < stop - 1; j++) if (data[j] == 0) { ok = false; break; }
                    if (!ok) break;
                    values.add(new String(data, start, stop - start - 1, StandardCharsets.UTF_8));
                }
                if (ok) return values;
                values.clear();
            }
        }
        throw new IOException("deploy subpath array is malformed");
    }

    private static long little(byte[] data, int offset, int width) {
        long value = 0;
        for (int i = width - 1; i >= 0; i--) value = (value << 8) | (data[offset + i] & 0xffL);
        return value;
    }

    /* ---- validation; mirrors linuxd's accepted ref/subpath syntax ---- */

    static boolean validRef(String ref) {
        if (ref == null || ref.length() >= 512) return false;
        int parts = 0, length = 0;
        for (int i = 0; i < ref.length(); i++) {
            char c = ref.charAt(i);
            if (c == '/') {
                if (length == 0 || (length == 1 && ref.charAt(i - 1) == '.')
                        || (length == 2 && ref.charAt(i - 1) == '.' && ref.charAt(i - 2) == '.'))
                    return false;
                parts++; length = 0;
            } else {
                if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9')
                        && c != '.' && c != '_' && c != '-') return false;
                length++;
            }
        }
        if (length == 0 || (length == 1 && ref.charAt(ref.length() - 1) == '.')
                || (length == 2 && ref.charAt(ref.length() - 1) == '.' && ref.charAt(ref.length() - 2) == '.'))
            return false;
        return parts == 3;
    }

    private static boolean validSubpath(String path) {
        if (path == null || path.isEmpty() || path.length() >= 256) return false;
        int start = path.charAt(0) == '/' ? 1 : 0;
        if (start == path.length()) return false;
        for (int i = start; i < path.length(); ) {
            int end = path.indexOf('/', i);
            if (end < 0) end = path.length();
            int n = end - i;
            if (n == 0 || (n == 1 && path.charAt(i) == '.')
                    || (n == 2 && path.charAt(i) == '.' && path.charAt(i + 1) == '.')) return false;
            for (int j = i; j < end; j++) if (path.charAt(j) < 32 || path.charAt(j) == 127) return false;
            if (end == path.length()) break;
            i = end + 1;
            if (i == path.length()) return false;
        }
        return true;
    }

    private static File[] entries(File directory) {
        return directory.isDirectory() ? directory.listFiles() : null;
    }

    private static boolean symlink(File file) {
        Path path = file.toPath();
        return Files.isSymbolicLink(path);
    }

    private DeployManifest() { }
}
