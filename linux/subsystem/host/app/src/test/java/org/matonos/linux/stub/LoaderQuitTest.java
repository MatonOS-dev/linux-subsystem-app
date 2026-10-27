package org.matonos.linux.stub;

public final class LoaderQuitTest {
    private static final class FakeProcess extends Process {
        boolean destroyed;
        @Override public java.io.OutputStream getOutputStream() { return java.io.OutputStream.nullOutputStream(); }
        @Override public java.io.InputStream getInputStream() { return java.io.InputStream.nullInputStream(); }
        @Override public java.io.InputStream getErrorStream() { return java.io.InputStream.nullInputStream(); }
        @Override public int waitFor() { return 0; }
        @Override public int exitValue() { return 0; }
        @Override public void destroy() { destroyed = true; }
    }

    private static void expect(boolean actual, boolean expected, String message) {
        if (actual != expected) throw new AssertionError(message);
    }

    public static void main(String[] args) {
        FakeProcess loader = new FakeProcess();
        expect(LoaderQuit.request(loader, false), true, "Quit did not request loader termination");
        expect(loader.destroyed, true, "Quit bypassed loader process exit");
        expect(LoaderQuit.request(loader, true), false, "Quit acted after loader exit");
        expect(LoaderQuit.request(null, false), false, "Quit acted without a loader");
        System.out.println("PASS: Quit requests loader exit and ignores an absent or exited loader");
    }
}
