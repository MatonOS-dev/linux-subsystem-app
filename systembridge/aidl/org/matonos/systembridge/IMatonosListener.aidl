package org.matonos.systembridge;

/** Event callback for daemon subscriptions. */
interface IMatonosListener {
    void onEvent(String target, String topic, String jsonEvent);
}
