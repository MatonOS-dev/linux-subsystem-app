import org.matonos.linux.stub.DeployManifest;

/** Every Flatpak ref must map to a name StubGenerator accepts (numeric branches included). */
public final class PackageNameTest {
    // Port of FrameworkParsingPackageUtils.validateName(name, true, true):
    // ASCII letters begin each dot-delimited segment; following chars may also be digits/underscore.
    private static boolean frameworkValid(String name) {
        if (name == null || name.length() > 255) return false;
        boolean separator = false, front = true;
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) { front = false; continue; }
            if (!front && ((c >= '0' && c <= '9') || c == '_')) continue;
            if (c == '.') { separator = true; front = true; continue; }
            return false;
        }
        return separator;
    }
    public static void main(String[] args) throws Exception {
        String[] refs = {
            "app/org.mozilla.firefox/x86_64/stable",
            "runtime/org.freedesktop.Platform/x86_64/25.08",
            "runtime/org.freedesktop.Platform.GL.default/x86_64/25.08-extra",
            "runtime/org.gnome.Platform/x86_64/49",
            "runtime/org.kde.Platform/x86_64/6.10",
            "runtime/org.freedesktop.Platform.Locale/x86_64/25.08",
        };
        int failed = 0;
        for (String ref : refs) {
            String name = DeployManifest.packageForPublic(ref);
            boolean ok = frameworkValid(name);
            if (!ok) failed++;
            System.out.println((ok ? "ok   " : "FAIL ") + ref + " -> " + name);
        }
        if (!DeployManifest.packageForPublic(refs[1]).equals(DeployManifest.packageForPublic(refs[1])))
            throw new AssertionError("package name is not deterministic");
        if (failed != 0) throw new AssertionError(failed + " refs map to invalid package names");
        System.out.println("PASS package names for " + refs.length + " Flatpak refs");
    }
}
