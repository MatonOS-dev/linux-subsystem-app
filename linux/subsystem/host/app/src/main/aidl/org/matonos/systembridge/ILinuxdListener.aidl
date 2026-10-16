package org.matonos.systembridge;

oneway interface ILinuxdListener {
    void onEvent(String topic, String json);
}
