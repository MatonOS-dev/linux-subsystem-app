package org.matonos.linux.stub;

import android.os.ParcelFileDescriptor;

/** Device-only diagnostic entry point for isolated regular-file path maps. */
interface IExtraProbe {
    void run(in ParcelFileDescriptor usr, in ParcelFileDescriptor app);
}
