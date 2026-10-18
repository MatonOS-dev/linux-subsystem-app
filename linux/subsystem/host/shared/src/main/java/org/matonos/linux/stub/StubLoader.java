package org.matonos.linux.stub;

import android.app.AppComponentFactory;
import android.content.pm.ApplicationInfo;
import android.os.Build;
import android.util.Log;

import dalvik.system.PathClassLoader;

import java.io.File;
import java.io.IOException;

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
    @Override
    public ClassLoader instantiateClassLoader(ClassLoader cl, ApplicationInfo aInfo) {
        String host = hostApk(aInfo);
        if (host == null) {
            Log.e(TAG, "No trusted installed host APK for " + aInfo.packageName);
            return cl;
        }
        String natives = host + "!/lib/" + Build.SUPPORTED_ABIS[0];
        return new PathClassLoader(host, natives, Object.class.getClassLoader());
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
            if (new File(canonical).getName().equals("base.apk")) return canonical;
        }
        return null;
    }

}
