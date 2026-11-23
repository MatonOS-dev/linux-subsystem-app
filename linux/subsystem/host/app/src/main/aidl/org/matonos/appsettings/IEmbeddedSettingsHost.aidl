package org.matonos.appsettings;

import android.view.SurfaceControlViewHost.SurfacePackage;

oneway interface IEmbeddedSettingsHost {
    void setSurface(in SurfacePackage p);
    void setPreferredHeight(int heightPx);
}
