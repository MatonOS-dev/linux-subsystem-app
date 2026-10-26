package org.matonos.linux.stub;

import android.os.ParcelFileDescriptor;

/** Device-only diagnostic entry point for the isolated apply_extra design. */
interface IExtraProbe {
    void run(in ParcelFileDescriptor usr, in ParcelFileDescriptor app);
}
