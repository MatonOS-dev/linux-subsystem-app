package org.matonos.linux.store;

import java.util.List;

/** Host-testable policy for the optional, lower-priority Flathub Beta remote. */
public final class FlathubBetaPolicy {
    public static final String REMOTE = "flathub-beta";
    public static final String DESCRIPTOR = "https://dl.flathub.org/beta-repo/flathub-beta.flatpakrepo";
    // The source sorts remote candidates by descending priority.
    public static final int STABLE_PRIORITY = 2;
    public static final int BETA_PRIORITY = 1;
    private FlathubBetaPolicy() {}

    public interface RemoteActions {
        void enable() throws Exception;
        void remove() throws Exception;
    }

    public static boolean parseEnabled(String value) {
        if (!"true".equals(value) && !"false".equals(value)) throw new IllegalArgumentException("Expected a boolean value");
        return Boolean.parseBoolean(value);
    }

    public static String setEnabled(boolean enabled, List<String> installedApps, RemoteActions actions) throws Exception {
        if (enabled) { actions.enable(); return null; }
        String error = removalError(installedApps);
        if (error != null) return error;
        actions.remove();
        return null;
    }

    public static String removalError(List<String> installedApps) {
        if (installedApps.isEmpty()) return null;
        return "Cannot disable Flathub Beta while these apps are installed: " + String.join(", ", installedApps);
    }
}
