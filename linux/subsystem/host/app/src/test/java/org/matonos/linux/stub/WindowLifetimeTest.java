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
        expect(lifetime.destroyed(first), false, "One of two windows was mistaken for the last window");
        expect(lifetime.destroyed(second), true, "Last close did not report the windowless state");
        expect(lifetime.destroyed(second), false, "Duplicate destruction reported a second last window");

        lifetime.opened(first);
        expect(lifetime.destroyed(first), true, "Configuration recreation did not report the temporary windowless state");
        lifetime.opened(replacement);
        expect(lifetime.destroyed(replacement), true, "Recreated window's close did not report the windowless state");

        lifetime.opened(first);
        lifetime.opened(replacement);
        expect(lifetime.destroyed(first), false, "Overlapping recreation was mistaken for the last window");
        expect(lifetime.destroyed(replacement), true, "Last recreated activity did not report the windowless state");

        lifetime.opened(first);
        lifetime.opened(first);
        expect(lifetime.destroyed(first), true, "Duplicate registration leaked a window");
        System.out.println("PASS: windowless notification state, multiple windows, recreation and duplicate callbacks");
    }
}
