package org.matonos.store.source;

/** Terminal PackageInstaller result of one commit; error is null on success. */
oneway interface ICommitResult {
    void result(boolean ok, String error);
}
