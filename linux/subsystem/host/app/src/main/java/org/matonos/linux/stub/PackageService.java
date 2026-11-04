package org.matonos.linux.stub;

/** Carries the flatpak.PACKAGE filter for package visibility; never does anything. */
public final class PackageService extends android.app.Service {
    @Override public android.os.IBinder onBind(android.content.Intent intent) { return null; }
}
