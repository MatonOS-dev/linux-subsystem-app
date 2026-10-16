package org.matonos.linux;

import org.matonos.linux.ILinuxAppStoreCallback;

/** Kept in sync with the rn-apps copy; keep the transaction order identical. */
interface ILinuxAppStore {
    void install(String ref, String remote, String operationId, ILinuxAppStoreCallback callback);
    void update(String ref, String operationId, ILinuxAppStoreCallback callback);
    void repair(String ref,String operationId,ILinuxAppStoreCallback callback);
    void uninstall(String ref, String operationId, ILinuxAppStoreCallback callback);
    String listInstalled();
    String listRemotes();
    void addRemote(String name, String url, String operationId, ILinuxAppStoreCallback callback);
    void removeRemote(String name, String operationId, ILinuxAppStoreCallback callback);
    void cancel(String operationId);
}
