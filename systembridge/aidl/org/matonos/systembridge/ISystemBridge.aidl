package org.matonos.systembridge;

import android.os.Bundle;
import org.matonos.systembridge.IMatonosListener;

/** MatonOS privileged bridge API. API version/hash are kept stable in-band. */
interface ISystemBridge {
    int getBridgeApiVersion();
    String getBridgeApiHash();
    boolean injectBackKey();
    String getBridgeStatus();
    boolean ensureShellOverlayAccess();
    // JSON array of {taskId, packageName, windowingMode}, current user, MRU first.
    String getRecentTasks(int maxTasks);
    boolean moveTaskToFront(int taskId);
    boolean setTaskFullscreen(int taskId);
    boolean removeRecentTask(int taskId);
    /** Flatpak management resolves a ref to a signed stub in the calling user.
     * Optional concrete userId requires cross-user permission when different;
     * raw installation UIDs are never accepted from apps. */
    String call(String target, String command, String jsonArgs);
    void subscribe(String target, String topic, IMatonosListener listener);
    void unsubscribe(String target, String topic, IMatonosListener listener);
    boolean navigateBack(boolean longPress);
    boolean navigateHome();
    boolean navigateRecents();
    byte[] getRecentTaskThumbnail(int taskId);
    // The provider is preset to Shelf but disabled until a user selects it.
    String getNavigationBarProviderState();
    // Verified stub commit info: returns JSON with appCommit, runtimeRef, runtimeCommit
    String getFlatpakStubCommits(int uid, String ref);
}
