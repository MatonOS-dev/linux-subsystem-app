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
    String call(String target, String command, String jsonArgs);
    void subscribe(String target, String topic, IMatonosListener listener);
    void unsubscribe(String target, String topic, IMatonosListener listener);
    boolean navigateBack(boolean longPress);
    boolean navigateHome();
    boolean navigateRecents();
    byte[] getRecentTaskThumbnail(int taskId);
    // The provider is preset to Shelf but disabled until a user selects it.
    String getNavigationBarProviderState();
    String launchFlatpak(String ref, in @nullable android.os.ParcelFileDescriptor x11Directory, @nullable String x11Display);
    String getFlatpakLaunchStatus(String ref);
    // Host-only attestation of an installed generated stub's UID, ref and signer.
    boolean isFlatpakStub(int uid, String ref);
    android.os.ParcelFileDescriptor[] createDnsForwarderSockets(String address, int stubUid, String ref);
    String launchOwnedFlatpak(String ref, in @nullable android.os.ParcelFileDescriptor x11Directory, @nullable String x11Display, @nullable String dnsForwarder, int stubUid, int stubPid, in android.os.ParcelFileDescriptor lifeline);
    // Verified stub commit info: returns JSON with appCommit, runtimeRef, runtimeCommit
    String getFlatpakStubCommits(int uid, String ref);
}
