package org.matonos.linux.stub;

import android.os.ParcelFileDescriptor;

/**
 * One package's Flatpak {@code apply_extra} runner.
 *
 * <p>Every generated app stub and every generated runtime package declares a
 * service implementing this interface in its own manifest, so the script runs
 * under the package's own UID and SELinux domain. Only the subsystem app may
 * call it; the implementation checks the calling UID against
 * {@code org.matonos.linux}'s and refuses every other caller.
 *
 * <p>{@code applyExtra} writes {@link org.matonos.linux.stub.ExtraProtocol}'s
 * status byte and, for STATUS_OK, a ustar archive of the applied extra tree
 * into {@code output}.
 */
interface IExtraExec {
    void applyExtra(in ParcelFileDescriptor output);
}
