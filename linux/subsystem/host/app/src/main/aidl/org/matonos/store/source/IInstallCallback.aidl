package org.matonos.store.source;

import org.matonos.store.source.ICommitResult;

/**
 * Install progress from a source. The store commits every APK itself; sources
 * never install. One install may commit several packages (dependencies
 * first): the source sends one commit per package and waits for its result
 * before committing anything that depends on it.
 */
oneway interface IInstallCallback {
    void progress(String operationId, String phase, int percent, long bytesDone, long bytesTotal);
    /**
     * One PackageInstaller session for packageName. names[i] is the APK file
     * name of files[i] in the session (base.apk, split_<name>.apk). With
     * base.apk it is a full install or update; without it the splits are added
     * to (or replace same-named splits of) the installed package. removeSplits
     * are split names to drop in the same session.
     */
    void commit(String operationId, String packageName, in String[] names,
            in ParcelFileDescriptor[] files, in String[] removeSplits, ICommitResult result);
    /** The whole install or update finished. */
    void done(String operationId);
    void failed(String operationId, String error);
}
