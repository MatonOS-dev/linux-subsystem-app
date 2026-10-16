package org.matonos.systembridge;

import org.matonos.systembridge.ICompositorInstallCallback;

/** Private host install-worker endpoint; callers must be the host UID. */
interface ICompositorInstall {
    /** Starts the authorized transaction and returns immediately; the worker
     * reports through callback. Rejects a second concurrent operation. */
    void startInstall(ICompositorInstallCallback callback, String operationId, String appRef, String remote);
    /** Stops the worker for operationId and releases its wake lock and DNS
     * service. The subsystem app owns staging cleanup. */
    void cancelInstall(String operationId);
}
