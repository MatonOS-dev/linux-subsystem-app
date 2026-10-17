import org.matonos.linux.stub.DeployManifest;

/** Every Flatpak ref must map to a name StubGenerator accepts (numeric branches included). */
public final class PackageNameTest {
    private static final String VALID = "[A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*)+";
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
            boolean ok = name.matches(VALID) && name.length() <= 255;
            if (!ok) failed++;
            System.out.println((ok ? "ok   " : "FAIL ") + ref + " -> " + name);
        }
        if (!DeployManifest.packageForPublic(refs[1]).equals(DeployManifest.packageForPublic(refs[1])))
            throw new AssertionError("package name is not deterministic");
        if (failed != 0) throw new AssertionError(failed + " refs map to invalid package names");
        System.out.println("PASS package names for " + refs.length + " Flatpak refs");
    }
}
