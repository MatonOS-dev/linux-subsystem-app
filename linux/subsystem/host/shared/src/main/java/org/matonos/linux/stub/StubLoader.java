package org.matonos.linux.stub;

import android.app.AppComponentFactory;
import android.app.Application;
import android.app.Activity;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.util.Log;

import dalvik.system.PathClassLoader;

import java.io.File;
import java.io.IOException;
import java.util.regex.Pattern;

/**
 * Generated stubs name this class as their {@code android:appComponentFactory}.
 *
 * <p>The stub reaches the host library through {@code <uses-library>}. This
 * factory replaces the platform loader
 * with one over the host APK PackageManager currently lists for the stub
 * ({@code sharedLibraryFiles}, which follows updates), parented to the boot
 * loader so no class resolves from the stale copy. Native libraries load from
 * the same APK. This class is loaded from the PackageManager-resolved host APK.
 *
 * <p>Trust: {@code sharedLibraryFiles} is populated by PackageManager from the
 * active package that provides the declared library. PackageManager enforces
 * the system package signing identity on updates.
 */
public final class StubLoader extends AppComponentFactory {
    private static final String TAG = "MatonStubLoader";
    private static final Pattern HOST_UPDATE = Pattern.compile(
            "^/data/app/~~[^/]+/org\\.matonos\\.linux-[^/]+/base\\.apk$");
    private boolean hostReady;
    @Override
    public ClassLoader instantiateClassLoader(ClassLoader cl, ApplicationInfo aInfo) {
        String host = hostApk(aInfo);
        if (host == null) {
            Log.e(TAG, "No trusted installed host APK for " + aInfo.packageName);
            hostReady = false;
            return cl;
        }
        hostReady = true;
        String natives = host + "!/lib/" + Build.SUPPORTED_ABIS[0];
        return new PathClassLoader(host, natives, Object.class.getClassLoader());
    }

    @Override public Application instantiateApplication(ClassLoader cl, String className)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        return hostReady ? super.instantiateApplication(cl, className) : new Application();
    }

    @Override public Activity instantiateActivity(ClassLoader cl, String className, Intent intent)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        return hostReady ? super.instantiateActivity(cl, className, intent) : new HostPreparingActivity();
    }

    @Override public Service instantiateService(ClassLoader cl, String className, Intent intent)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        return hostReady ? super.instantiateService(cl, className, intent) : new InactiveService();
    }

    @Override public BroadcastReceiver instantiateReceiver(ClassLoader cl, String className, Intent intent)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        return hostReady ? super.instantiateReceiver(cl, className, intent) : new InactiveReceiver();
    }

    @Override public ContentProvider instantiateProvider(ClassLoader cl, String className)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        return hostReady ? super.instantiateProvider(cl, className) : new InactiveProvider();
    }

    private static String hostApk(ApplicationInfo aInfo) {
        if (aInfo.sharedLibraryFiles == null) return null;
        for (String path : aInfo.sharedLibraryFiles) {
            String canonical;
            try {
                canonical = new File(path).getCanonicalPath();
            } catch (IOException e) {
                continue;
            }
            if (HOST_UPDATE.matcher(canonical).matches()) return canonical;
        }
        return null;
    }

}

/** Small fallback components used while the bootstrap APK replaces itself. */
final class HostPreparingActivity extends Activity {
    @Override protected void onCreate(android.os.Bundle state) {
        super.onCreate(state);
        android.widget.TextView message = new android.widget.TextView(this);
        message.setText("The Linux subsystem is being prepared. Open the app again in a moment.");
        message.setGravity(android.view.Gravity.CENTER);
        message.setPadding(24, 24, 24, 24);
        setContentView(message);
        Intent retry = new Intent("org.matonos.linux.BOOTSTRAP")
                .setComponent(new android.content.ComponentName("org.matonos.linux",
                        "org.matonos.linux.bootstrap.BootstrapReceiver"));
        sendBroadcast(retry);
    }

    @Override public void onBackPressed() { finish(); }
}

final class InactiveService extends Service {
    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        stopSelf(startId);
        return START_NOT_STICKY;
    }
    @Override public android.os.IBinder onBind(Intent intent) { return null; }
}

final class InactiveReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) { }
}

final class InactiveProvider extends ContentProvider {
    @Override public boolean onCreate() { return true; }
    @Override public Cursor query(Uri uri, String[] projection, String selection,
            String[] selectionArgs, String sortOrder) { return null; }
    @Override public String getType(Uri uri) { return null; }
    @Override public Uri insert(Uri uri, ContentValues values) { return null; }
    @Override public int delete(Uri uri, String selection, String[] selectionArgs) { return 0; }
    @Override public int update(Uri uri, ContentValues values, String selection,
            String[] selectionArgs) { return 0; }
}
