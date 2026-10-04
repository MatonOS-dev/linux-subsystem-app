import org.matonos.linuxhost.stubgen.StubGenerator;

/** Tests for OSTree commit checksum validation and CommitInfo construction. */
public final class CommitValidationTest {
    private static final String VALID_COMMIT = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    private static final String VALID_RUNTIME_REF = "runtime/org.freedesktop.Platform/x86_64/stable";
    private static final String VALID_APP_REF = "app/org.example.Editor/x86_64/stable";

    private static void assertTrue(boolean b, String msg) { if (!b) throw new AssertionError(msg); }
    private static void assertFalse(boolean b, String msg) { if (b) throw new AssertionError(msg); }

    public static void main(String[] args) throws Exception {
        // validCommit: accept valid 64-char lowercase hex
        assertTrue(StubGenerator.validCommit(VALID_COMMIT), "valid commit should be accepted");
        assertTrue(StubGenerator.validCommit("0000000000000000000000000000000000000000000000000000000000000000"), "all zeros");
        assertTrue(StubGenerator.validCommit("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"), "all f's");
        assertTrue(StubGenerator.validCommit("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"), "mixed hex");

        // validCommit: reject invalid values
        assertFalse(StubGenerator.validCommit(null), "null should be rejected");
        assertFalse(StubGenerator.validCommit(""), "empty should be rejected");
        assertFalse(StubGenerator.validCommit("abc"), "too short should be rejected");
        assertFalse(StubGenerator.validCommit(VALID_COMMIT + "0"), "too long should be rejected");
        assertFalse(StubGenerator.validCommit(VALID_COMMIT.toUpperCase()), "uppercase should be rejected");
        assertFalse(StubGenerator.validCommit("abcdefgh0123456789abcdef0123456789abcdef0123456789abcdef01234567"), "g is not hex");
        assertFalse(StubGenerator.validCommit("abcdef01 23456789abcdef0123456789abcdef0123456789abcdef01234567"), "space rejected");

        // CommitInfo: accept valid data
        StubGenerator.CommitInfo info = new StubGenerator.CommitInfo(VALID_COMMIT, VALID_RUNTIME_REF, VALID_COMMIT);
        assertTrue(VALID_COMMIT.equals(info.appCommit), "appCommit stored");
        assertTrue(VALID_RUNTIME_REF.equals(info.runtimeRef), "runtimeRef stored");
        assertTrue(VALID_COMMIT.equals(info.runtimeCommit), "runtimeCommit stored");

        // CommitInfo: reject invalid app commit
        try {
            new StubGenerator.CommitInfo("bad", VALID_RUNTIME_REF, VALID_COMMIT);
            throw new AssertionError("should reject bad app commit");
        } catch (IllegalArgumentException expected) { }

        // CommitInfo: reject invalid runtime ref
        try {
            new StubGenerator.CommitInfo(VALID_COMMIT, "app/org.example.Foo/x86_64/stable", VALID_COMMIT);
            throw new AssertionError("should reject app ref as runtime ref");
        } catch (IllegalArgumentException expected) { }

        try {
            new StubGenerator.CommitInfo(VALID_COMMIT, "not-a-ref", VALID_COMMIT);
            throw new AssertionError("should reject invalid runtime ref");
        } catch (IllegalArgumentException expected) { }

        // CommitInfo: reject invalid runtime commit
        try {
            new StubGenerator.CommitInfo(VALID_COMMIT, VALID_RUNTIME_REF, "");
            throw new AssertionError("should reject empty runtime commit");
        } catch (IllegalArgumentException expected) { }

        try {
            new StubGenerator.CommitInfo(VALID_COMMIT, VALID_RUNTIME_REF, VALID_COMMIT.toUpperCase());
            throw new AssertionError("should reject uppercase runtime commit");
        } catch (IllegalArgumentException expected) { }

        // CommitInfo: accept different values for app and runtime commits
        String differentCommit = "1111111111111111111111111111111111111111111111111111111111111111";
        StubGenerator.CommitInfo info2 = new StubGenerator.CommitInfo(VALID_COMMIT, VALID_RUNTIME_REF, differentCommit);
        assertTrue(VALID_COMMIT.equals(info2.appCommit), "appCommit stored (different)");
        assertTrue(differentCommit.equals(info2.runtimeCommit), "runtimeCommit stored (different)");

        System.out.println("Commit validation checks passed");
    }
}