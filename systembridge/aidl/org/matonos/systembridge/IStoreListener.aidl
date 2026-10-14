package org.matonos.systembridge;
oneway interface IStoreListener {
    void onProgress(String operationId, String state, int percent, String message);
    void onFinished(String operationId, boolean ok, String error);
}
