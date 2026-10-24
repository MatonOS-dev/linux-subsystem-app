package org.matonos.linux;

import android.app.Activity;
import android.os.Bundle;

/**
 * Stubs start this for a result before their first EROFS mount: it pins the
 * stub certificate if boot has not done so yet and returns RESULT_OK once the
 * daemon holds it.
 */
public final class ErofsPinActivity extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        new Thread(() -> {
            boolean pinned = ErofsPin.ensure();
            runOnUiThread(() -> {
                setResult(pinned ? RESULT_OK : RESULT_CANCELED);
                finish();
            });
        }).start();
    }
}
