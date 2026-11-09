package org.matonos.tray;

import android.os.IBinder;
import android.view.SurfaceControlViewHost.SurfacePackage;
import android.graphics.Rect;
import org.matonos.tray.ITrayListener;
import org.matonos.tray.IPillCallback;

interface ITrayHost {
    void register(ITrayListener listener);
    void unregister(ITrayListener listener);
    oneway void activate(String key, in Rect anchor);
    oneway void openMenu(String key, in Rect anchor);
    SurfacePackage attachPill(int displayId, IBinder hostToken, int heightPx, IPillCallback callback);
    void detachPill();
    void setPillOrigin(int screenX, int screenY);
}
