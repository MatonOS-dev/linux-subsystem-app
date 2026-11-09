package org.matonos.tray;

import android.graphics.Rect;
import org.matonos.tray.ITrayListener;

interface ITrayHost {
    void register(ITrayListener listener);
    void unregister(ITrayListener listener);
    oneway void activate(String key, in Rect anchor);
    oneway void openMenu(String key, in Rect anchor);
}
