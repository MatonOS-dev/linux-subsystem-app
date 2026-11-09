package org.matonos.linux;

import android.os.ParcelFileDescriptor;
import org.matonos.linux.IStubIcons;

interface IDBusService {
    void attach(in ParcelFileDescriptor socket);
    void attachWithIcons(in ParcelFileDescriptor socket, IStubIcons icons);
}
