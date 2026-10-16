package org.matonos.systembridge;

/** Private host install-worker callback; transaction identity is checked by the host. */
oneway interface ICompositorInstallCallback {
    void onProgress(String operationId, String phase, int percent, String line);
    /** Bounded channel JSON manifest with the exact staged-install schema. */
    void onComplete(String operationId, String manifest);
    void onFailed(String operationId, String error);
}
