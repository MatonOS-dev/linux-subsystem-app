package org.matonos.linux.stub;

import android.os.ParcelFileDescriptor;

/** Runs Flatpak's offline deploy path in an isolated UID using a streamed tar input. */
interface IIsolatedFlatpakInstall {
    void deploy(in ParcelFileDescriptor input, in ParcelFileDescriptor result,
            String remote, String ref, String commit,
            in String[] subpaths);
}
