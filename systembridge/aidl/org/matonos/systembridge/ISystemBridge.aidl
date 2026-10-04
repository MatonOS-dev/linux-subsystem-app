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
    String launchFlatpak(String ref, in android.os.ParcelFileDescriptor runtimeDirectory, in @nullable android.os.ParcelFileDescriptor x11Directory, @nullable String x11Display);
    String getFlatpakLaunchStatus(String ref);
    // Host-only attestation of an installed generated stub's UID, ref and signer.
    boolean isFlatpakStub(int uid, String ref);
    String launchOwnedFlatpak(String ref, in android.os.ParcelFileDescriptor runtimeDirectory, in @nullable android.os.ParcelFileDescriptor x11Directory, @nullable String x11Display, int stubUid, int stubPid, in android.os.ParcelFileDescriptor lifeline);
    // Privileged installer; returns runtime/extra PackageInstaller session ID.
    int installFlatpakImagePackages(String ref, in android.os.ParcelFileDescriptor runtimeApk, in android.os.ParcelFileDescriptor appApk);
    int installFlatpakExtra(String ref, in android.os.ParcelFileDescriptor extraApk);
    // Verified PackageManager-derived launch record, code/extra/runtime lines.
    String getFlatpakImagePackages(int uid, String ref);
    int reserveFlatpakRuntimeVersion(String runtimeRef);
}
