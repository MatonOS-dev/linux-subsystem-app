package org.matonos.systembridge;

interface ILinuxdListener {
    void onEvent(String topic, String json);
}
