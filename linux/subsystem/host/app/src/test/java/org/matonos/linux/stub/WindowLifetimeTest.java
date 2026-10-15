package org.matonos.linux.stub;

public final class WindowLifetimeTest {
    private static void expect(boolean actual, boolean expected, String message) {
        if (actual != expected) throw new AssertionError(message);
    }

    public static void main(String[] args) {
        WindowLifetime lifetime = new WindowLifetime();
        Object first = new Object(), second = new Object(), replacement = new Object();
        lifetime.opened(first);
        lifetime.opened(second);
        expect(lifetime.destroyed(first, true, false), false, "One of two windows closed the session");
        expect(lifetime.destroyed(second, true, false), true, "Last explicit close did not end the session");
        expect(lifetime.destroyed(second, true, false), false, "Duplicate destruction ended it twice");

        lifetime.opened(first);
        expect(lifetime.destroyed(first, false, true), false, "Configuration recreation ended the session");
        lifetime.opened(replacement);
        expect(lifetime.destroyed(replacement, true, false), true, "Recreated window's close did not end the session");

        lifetime.opened(first);
        lifetime.opened(replacement);
        expect(lifetime.destroyed(first, false, true), false, "Overlapping recreation ended the session");
        expect(lifetime.destroyed(replacement, false, false), false, "Non-finishing destruction ended the session");

        lifetime.opened(first);
        lifetime.opened(first);
        expect(lifetime.destroyed(first, true, false), true, "Duplicate registration leaked a window");
        System.out.println("PASS: last-window close, multiple windows, recreation and duplicate callbacks");
    }
}
