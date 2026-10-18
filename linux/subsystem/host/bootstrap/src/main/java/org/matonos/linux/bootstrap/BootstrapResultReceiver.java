package org.matonos.linux.bootstrap;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.util.Log;

/** Kept at the same component name in the replacement host APK. */
public final class BootstrapResultReceiver extends BroadcastReceiver {
    static final String ACTION_RESULT = "org.matonos.linux.INSTALL_RESULT";

    @Override public void onReceive(Context context, Intent intent) {
        int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        Log.i("MatonBootstrap", "Subsystem install result status=" + status + " message=" +
                intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
    }
}
