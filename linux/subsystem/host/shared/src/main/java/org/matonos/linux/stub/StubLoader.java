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
 * <p>This class is the generated stub's own dex (StubGenerator embeds it);
 * stubs declare no {@code <uses-library>}, because the library path
 * PackageManager records for a stub does not follow host updates. The stub's
 * class loader is a {@link HostClassLoader} that, on the first component class
 * lookup, asks PackageManager for the current {@code org.matonos.linux} APK and
 * delegates to a loader over it (natives from the same APK, parented to the
 * boot loader). The plain Application exists and is attached by then; stubs
 * declare none.
 *
 * <p>Trust: only an installed update of the host package at its
 * {@code /data/app} path is accepted; PackageManager enforces the system
 * package's signing identity on updates. While only the bootstrap is active,
 * stubs get small fallback components that ask the bootstrap to install.
 */
public final class StubLoader extends AppComponentFactory {
    private static final String TAG = "MatonStubLoader";
    private static final String HOST_PACKAGE = "org.matonos.linux";
    private static final Pattern HOST_UPDATE = Pattern.compile(
            "^/data/app/~~[^/]+/org\\.matonos\\.linux-[^/]+/base\\.apk$");
    private static volatile Application application;
    /** Static: LoadedApk creates this factory twice (before and after instantiateClassLoader). */
    private static final HostClassLoader hostLoader = new HostClassLoader();

    @Override
    public ClassLoader instantiateClassLoader(ClassLoader cl, ApplicationInfo aInfo) {
        return hostLoader;
    }

    @Override public Application instantiateApplication(ClassLoader cl, String className)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        Application app = new Application();
        application = app;
        return app;
    }

    @Override public Activity instantiateActivity(ClassLoader cl, String className, Intent intent)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        return hostLoader.ready() ? super.instantiateActivity(hostLoader, className, intent) : new HostPreparingActivity();
    }

    @Override public Service instantiateService(ClassLoader cl, String className, Intent intent)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        return hostLoader.ready() ? super.instantiateService(hostLoader, className, intent) : new InactiveService();
    }

    @Override public BroadcastReceiver instantiateReceiver(ClassLoader cl, String className, Intent intent)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        return hostLoader.ready() ? super.instantiateReceiver(hostLoader, className, intent) : new InactiveReceiver();
    }

    @Override public ContentProvider instantiateProvider(ClassLoader cl, String className)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        return hostLoader.ready() ? super.instantiateProvider(hostLoader, className) : new InactiveProvider();
    }

    /** The installed host update's APK, or null while only the bootstrap is active. */
    static String hostApk(Context context) {
        try {
            ApplicationInfo host = context.getPackageManager().getApplicationInfo(HOST_PACKAGE, 0);
            if ((host.flags & ApplicationInfo.FLAG_UPDATED_SYSTEM_APP) == 0) return null;
            String canonical = new File(host.sourceDir).getCanonicalPath();
            return HOST_UPDATE.matcher(canonical).matches() ? canonical : null;
        } catch (Exception e) {
            Log.e(TAG, "Host lookup failed", e);
            return null;
        }
    }

    /** Resolves the host on first use; the stub process keeps that host until it exits. */
    static final class HostClassLoader extends ClassLoader {
        private PathClassLoader host;
        private boolean resolved;

        HostClassLoader() { super(Object.class.getClassLoader()); }

        synchronized boolean ready() { return delegate() != null; }

        private synchronized PathClassLoader delegate() {
            if (resolved) return host;
            Application app = application;
            if (app == null) return null; // Before attach: retry on the next lookup.
            resolved = true;
            String apk = hostApk(app);
            if (apk == null) {
                Log.e(TAG, "No installed host for " + app.getPackageName());
                return null;
            }
            host = new PathClassLoader(apk, apk + "!/lib/" + Build.SUPPORTED_ABIS[0],
                    Object.class.getClassLoader());
            return host;
        }

        @Override protected Class<?> findClass(String name) throws ClassNotFoundException {
            PathClassLoader loader = delegate();
            // Before the host is known (factory re-creation, fallback components) our own
            // classes come from the stub dex that loaded this class.
            if (loader == null) return StubLoader.class.getClassLoader().loadClass(name);
            return loader.loadClass(name);
        }

        @Override protected java.net.URL findResource(String name) {
            PathClassLoader loader = delegate();
            return loader == null ? null : loader.getResource(name);
        }

        @Override protected java.util.Enumeration<java.net.URL> findResources(String name)
                throws IOException {
            PathClassLoader loader = delegate();
            return loader == null ? java.util.Collections.emptyEnumeration() : loader.getResources(name);
        }

        @Override protected String findLibrary(String libname) {
            PathClassLoader loader = delegate();
            return loader == null ? null : loader.findLibrary(libname);
        }
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
