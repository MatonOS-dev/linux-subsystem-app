package org.matonos.linux;

import android.os.IBinder;
import android.util.Log;

import java.security.MessageDigest;

import org.matonos.erofs.IErofsMounter;
import org.matonos.linuxhost.stubgen.StubGenerator;

/** Shares the stub signing certificate with matonos-erofsd; its pin lives only until reboot. */
final class ErofsPin {
    private static final String TAG = "ErofsPin";
    private static final String SERVICE = "matonos.erofs";
    private static volatile boolean pinned;

    private ErofsPin() {}

    /** True once the daemon holds the pin; false if there is no stub key yet or the daemon is down. */
    static synchronized boolean ensure() {
        if (pinned) return true;
        try {
            byte[] certificate = StubGenerator.getExistingSigningCertificate();
            if (certificate == null) return false;
            IErofsMounter mounter = IErofsMounter.Stub.asInterface(service());
            if (mounter == null) return false;
            mounter.pinStubCertificate(MessageDigest.getInstance("SHA-256").digest(certificate));
            pinned = true;
        } catch (Exception e) {
            Log.w(TAG, "cannot pin stub certificate", e);
        }
        return pinned;
    }

    // android.os.ServiceManager is hidden ("unsupported" list, still callable).
    private static IBinder service() throws Exception {
        return (IBinder) Class.forName("android.os.ServiceManager")
                .getMethod("getService", String.class).invoke(null, SERVICE);
    }
}
