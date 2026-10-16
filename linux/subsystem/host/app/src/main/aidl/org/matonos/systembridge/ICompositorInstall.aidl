package org.matonos.systembridge;

import org.matonos.systembridge.ICompositorInstallCallback;

/** Bridge-only compositor Flatpak installer endpoint.
 *
 * The authenticated System Bridge owns the transaction and resolves this
 * service's own application UID. The compositor performs the authorized pull,
 * deployment and apply_extra in its private staging installation and reports
 * progress and completion through the supplied callback. No caller-supplied
 * path, loader flag, environment or final identity crosses this API. */
interface ICompositorInstall {
    /** Starts the authorized transaction and returns immediately; the worker
     * reports through callback. Rejects a second concurrent operation. */
    void startInstall(ICompositorInstallCallback callback, String operationId, String appRef, String remote);
    /** Stops the worker for operationId and releases its wake lock and DNS
     * service. Staging cleanup remains linuxd's. */
    void cancelInstall(String operationId);
}
