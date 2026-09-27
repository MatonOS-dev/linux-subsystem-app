package org.matonos.systembridge;

import android.os.IBinder;
import org.matonos.systembridge.INavigationBarHostCallback;

/** Private IPC contract used by MatonOSClient's typed navigation provider adapter. */
interface INavigationBarProvider {
    void attach(IBinder hostToken, int displayId, int widthPx, int heightPx,
            INavigationBarHostCallback callback);
    void detach();
    /** Host tells the provider the dimensions applied after its preferred-height request. */
    oneway void onHostSizeChanged(int widthPx, int heightPx);
    oneway void setFallbackOverlayEnabled(boolean enabled);
    /** Top task changed: its package, and whether it is the home (launcher) task. */
    oneway void onForegroundChanged(String packageName, boolean isHome);
}
