package org.matonos.linux;

oneway interface ILinuxAppStoreCallback {
    void onProgress(String operationId, String phase, int percent, long bytesDone, long bytesTotal);
    void onResult(String operationId, boolean ok, String error);
}
