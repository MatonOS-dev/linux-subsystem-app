package org.matonos.systembridge;

import android.os.Bundle;

/** Provider-to-host attachment result. The bundle contains a SurfacePackage Parcelable. */
oneway interface INavigationBarHostCallback {
    void onSurfaceReady(in Bundle response);
}
