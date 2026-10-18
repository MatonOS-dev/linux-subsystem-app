package org.matonos.linux.bootstrap;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

public final class BootstrapActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        sendBroadcast(new Intent(this, BootstrapReceiver.class).setAction("org.matonos.linux.BOOTSTRAP"));
        finish();
    }
}
