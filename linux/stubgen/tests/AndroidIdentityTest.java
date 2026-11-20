import org.matonos.linuxhost.stubgen.AndroidIdentity;

public final class AndroidIdentityTest {
    private static void check(boolean value) { if (!value) throw new AssertionError(); }
    public static void main(String[] args) throws Exception {
        check(AndroidIdentity.packageName("org.mozilla.firefox").equals("flatpak.org.mozilla.firefox"));
        check(AndroidIdentity.packageName("com.valvesoftware.Steam").equals("flatpak.com.valvesoftware.Steam"));
        check(AndroidIdentity.packageName("io.github._foo-bar.App").equals("flatpak.io.github.v_foo_bar.App"));
        check(AndroidIdentity.splitName("x86_64","stable").equals("x86_64.stable"));
        check(AndroidIdentity.splitName("x86_64","25.08-extra").equals("x86_64.v25_08_extra"));
        check(AndroidIdentity.packageName("org.example.foo-bar").equals(AndroidIdentity.packageName("org.example.foo_bar")));
        try { AndroidIdentity.packageName("org..example"); throw new AssertionError("empty segment accepted"); }
        catch (java.io.IOException expected) { }
        System.out.println("PASS stable app-id package names, branch split names and deterministic collision identity");
    }
}
