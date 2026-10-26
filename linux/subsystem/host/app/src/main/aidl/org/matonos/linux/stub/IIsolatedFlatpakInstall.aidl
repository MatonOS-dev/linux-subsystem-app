package org.matonos.linux.stub;

import android.os.ParcelFileDescriptor;

/** Runs Flatpak's offline deploy path in an isolated UID using only passed directory fds. */
interface IIsolatedFlatpakInstall {
    void deploy(in ParcelFileDescriptor installation, in ParcelFileDescriptor repo,
            in ParcelFileDescriptor result, String remote, String ref, String commit,
            in String[] subpaths);
}
