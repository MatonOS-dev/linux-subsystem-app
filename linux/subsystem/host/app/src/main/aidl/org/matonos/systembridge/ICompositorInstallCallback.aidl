package org.matonos.systembridge;

/** Compositor-to-bridge result of one authorized transaction.
 *
 * The bridge authenticates every call against the compositor UID it resolved
 * for the operation, so a generic trusted app can never complete another
 * compositor transaction. */
oneway interface ICompositorInstallCallback {
    void onProgress(String operationId, String phase, int percent, String line);
    /** Bounded channel JSON manifest with the exact staged-install schema. */
    void onComplete(String operationId, String manifest);
    void onFailed(String operationId, String error);
}
