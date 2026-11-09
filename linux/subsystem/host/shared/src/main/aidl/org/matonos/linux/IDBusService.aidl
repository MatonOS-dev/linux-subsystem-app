package org.matonos.linux;

import android.os.ParcelFileDescriptor;

interface IDBusService {
    void attach(in ParcelFileDescriptor socket);
}
