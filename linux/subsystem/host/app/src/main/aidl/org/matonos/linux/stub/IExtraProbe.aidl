package org.matonos.linux.stub;

import android.os.ParcelFileDescriptor;

/** Device-only diagnostic entry point for isolated regular-file stream transport. */
interface IExtraProbe {
    void run(in ParcelFileDescriptor file);
}
