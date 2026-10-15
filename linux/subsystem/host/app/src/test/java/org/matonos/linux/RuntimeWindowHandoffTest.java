package org.matonos.linux;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;

/** Host regression test: exercise window callbacks without native/Android startup. */
public final class RuntimeWindowHandoffTest {
    private static final class Host implements PerAppRuntime.WindowHost {
        final List<Integer> opened = new ArrayList<>();
        final List<Integer> closed = new ArrayList<>();
        public void windowOpened(int id, int width, int height) {
            if (width != 640 || height != 400) throw new AssertionError("Lost window dimensions");
            opened.add(id);
        }
        boolean uriOpened, inhibited;
        public boolean openUri(android.content.Intent intent) { uriOpened=true; return true; }
        public void inhibitChanged(boolean active) { inhibited=active; }
        public void windowClosed(int id) { closed.add(id); }
    }

    public static void main(String[] args) throws Exception {
        // Skip the constructor's native compositor and session-bus startup.
        Class<?> unsafeClass = Class.forName("sun.misc.Unsafe");
        Field singleton = unsafeClass.getDeclaredField("theUnsafe");
        singleton.setAccessible(true);
        Object unsafe = singleton.get(null);
        PerAppRuntime runtime = (PerAppRuntime) unsafeClass.getMethod("allocateInstance", Class.class)
                .invoke(unsafe, PerAppRuntime.class);
        Field windows = PerAppRuntime.class.getDeclaredField("windows");
        windows.setAccessible(true);
        windows.set(runtime, new LinkedHashMap<Integer, int[]>());
        Method opened = PerAppRuntime.class.getDeclaredMethod("onNativeToplevel", int.class, int.class, int.class, int.class);
        Method closed = PerAppRuntime.class.getDeclaredMethod("onNativeToplevelClosed", int.class);
        opened.setAccessible(true);
        closed.setAccessible(true);
        opened.invoke(runtime, 1, 7, 640, 400); // event before an activity connects
        opened.invoke(runtime, 1, 8, 640, 400);
        Host root = new Host();
        runtime.setWindowHost(root, 0);
        if (!root.opened.equals(List.of(7))) throw new AssertionError("Root did not recover existing window");
        Host secondary = new Host();
        runtime.setWindowHost(secondary, 8);
        if (!secondary.opened.equals(List.of(8))) throw new AssertionError("Secondary adopted wrong window");
        Method open = PerAppRuntime.class.getDeclaredMethod("open", android.content.Intent.class);
        open.setAccessible(true);
        if (!Boolean.TRUE.equals(open.invoke(runtime, new Object[]{null})) || !secondary.uriOpened || root.uriOpened)
            throw new AssertionError("URI callback did not reach the current local host");
        Field inhibited = PerAppRuntime.class.getDeclaredField("inhibited");
        inhibited.setAccessible(true); inhibited.setBoolean(runtime, true);
        runtime.setWindowHost(secondary, 8);
        if (!secondary.inhibited) throw new AssertionError("Inhibit state was not replayed locally");
        runtime.clearWindowHost(root); // old activity must not clear its replacement
        closed.invoke(runtime, 8);
        if (!secondary.closed.equals(List.of(8))) throw new AssertionError("Replacement lost callback");
        closed.invoke(runtime, 7);
        Host reopened = new Host();
        runtime.setWindowHost(reopened, 0);
        if (!reopened.opened.isEmpty()) throw new AssertionError("Closed window replayed");
        opened.invoke(runtime, 1, 9, 640, 400);
        if (!reopened.opened.equals(List.of(9))) throw new AssertionError("New window callback lost");
        runtime.clearWindowHost(reopened);
        opened.invoke(runtime, 1, 10, 640, 400);
        if (reopened.opened.size() != 1) throw new AssertionError("Destroyed activity retained");
        System.out.println("PASS: existing-window handoff, saved IDs, closed-window removal and host replacement");
    }
}
