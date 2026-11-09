package org.matonos.linux;

import android.os.ParcelFileDescriptor;

interface IStubIcons {
    ParcelFileDescriptor openIcon(String iconThemePath, String iconName, int sizePx);
    boolean hasLiveWindow();
    void finishPlaceholderIfNoWindow();
}
