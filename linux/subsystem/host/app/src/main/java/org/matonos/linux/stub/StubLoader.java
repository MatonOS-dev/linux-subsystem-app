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
 * <p>The stub reaches the host library through {@code <uses-library>}, but the
 * platform builds that shared-library loader from the system image copy, even
 * after the host package is updated. This factory replaces the stub's loader
 * with one over the host APK PackageManager currently lists for the stub
 * ({@code sharedLibraryFiles}, which follows updates), parented to the boot
 * loader so no class resolves from the stale copy. Native libraries load from
 * the same APK. This class itself always comes from the image copy, so keep it
 * small and stable.
 *
 * <p>Trust: system_server supplies {@code sharedLibraryFiles}, and an update
 * of a system package must carry the system package's signature, so the only
 * accepted paths are the image copy and an installed update of the host
 * package. Anything else keeps the platform's default loader.
 */
public final class StubLoader extends AppComponentFactory {
    private static final String TAG = "MatonStubLoader";
    private static final String HOST_PACKAGE = "org.matonos.linux";
    /** Install path of the MatonLinuxSubsystem module (buildinfra/Android.bp). Keep in sync:
     *  a rename, partition change or loss of `privileged` there makes this check reject the
     *  image copy, and stubs then fall back to the platform loader without notice. */
    private static final String SYSTEM_APK = "/system_ext/priv-app/MatonLinuxSubsystem/MatonLinuxSubsystem.apk";

    @Override
    public ClassLoader instantiateClassLoader(ClassLoader cl, ApplicationInfo aInfo) {
        String host = hostApk(aInfo);
        if (host == null) {
            Log.e(TAG, "No trusted host APK for " + aInfo.packageName + "; using the image copy");
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
            if (canonical.equals(SYSTEM_APK) || isHostUpdate(canonical)) return canonical;
        }
        return null;
    }

    /** /data/app/~~<random>/org.matonos.linux-<random>/base.apk */
    private static boolean isHostUpdate(String path) {
        String[] parts = path.split("/");
        return parts.length == 6 && parts[0].isEmpty() && parts[1].equals("data")
                && parts[2].equals("app") && parts[3].startsWith("~~")
                && parts[4].startsWith(HOST_PACKAGE + "-") && parts[5].equals("base.apk");
    }
}
