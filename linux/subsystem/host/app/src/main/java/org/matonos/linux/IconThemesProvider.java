package org.matonos.linux;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Binder;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import org.matonos.erofs.IErofsMounter;

/** Read-only icon-theme access for signed MatonOS clients. */
public final class IconThemesProvider extends ContentProvider {
    private static final String TAG = "MatonIconThemes";
    private static final String REF_META = "org.matonos.linuxhost.FLATPAK_REF";
    private final Map<String, List<File>> roots = new HashMap<>();
    private final Map<String, File> packageRoots = new HashMap<>();
    private final Map<String, IBinder> mountTokens = new HashMap<>();
    private final android.content.BroadcastReceiver packages = new android.content.BroadcastReceiver() {
        @Override public void onReceive(android.content.Context context, android.content.Intent intent) {
            synchronized (IconThemesProvider.this) {
                roots.clear();
                packageRoots.clear();
                mountTokens.clear();
            }
        }
    };

    @Override public boolean onCreate() {
        android.content.IntentFilter filter = new android.content.IntentFilter();
        filter.addAction(android.content.Intent.ACTION_PACKAGE_ADDED);
        filter.addAction(android.content.Intent.ACTION_PACKAGE_REMOVED);
        filter.addAction(android.content.Intent.ACTION_PACKAGE_REPLACED);
        filter.addDataScheme("package");
        getContext().registerReceiver(packages, filter, android.content.Context.RECEIVER_NOT_EXPORTED);
        return true;
    }

    @Override public String getType(Uri uri) {
        if ("themes".equals(uri.getLastPathSegment())) return "vnd.android.cursor.dir/vnd.maton.icon-theme";
        List<String> parts = uri.getPathSegments();
        if (parts.size() != 5 || !"icon".equals(parts.get(0))) return null;
        try {
            List<File> iconRoots = installedRoots().get(parts.get(1));
            if (iconRoots == null) return null;
            File icon = IconThemeLookup.find(iconRoots, parts.get(1), parts.get(2),
                    Integer.parseInt(parts.get(3)), Integer.parseInt(parts.get(4)));
            if (icon == null) return null;
            return icon.getName().endsWith(".svg") ? "image/svg+xml" : "image/png";
        } catch (Exception error) {
            Log.e(TAG, "Cannot determine icon type theme=" + parts.get(1) + " path=" + uri, error);
            return null;
        }
    }

    @Override public Cursor query(Uri uri, String[] projection, String selection,
            String[] selectionArgs, String sortOrder) {
        if (!"themes".equals(uri.getLastPathSegment())) return null;
        MatrixCursor cursor = new MatrixCursor(new String[]{"theme"});
        try {
            for (String theme : installedRoots().keySet()) cursor.addRow(new Object[]{theme});
        } catch (Exception error) {
            Log.e(TAG, "Cannot enumerate installed icon themes", error);
        }
        return cursor;
    }

    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new FileNotFoundException("Icon themes are read-only");
        List<String> parts = uri.getPathSegments();
        if (parts.size() != 5 || !"icon".equals(parts.get(0))) throw new FileNotFoundException("Unknown icon URI: " + uri);
        String theme = parts.get(1), name = parts.get(2);
        int size, scale;
        try { size = Integer.parseInt(parts.get(3)); scale = Integer.parseInt(parts.get(4)); }
        catch (NumberFormatException error) { throw new FileNotFoundException("Invalid icon dimensions: " + uri); }
        try {
            List<File> iconRoots = installedRoots().get(theme);
            if (iconRoots == null) return null;
            File icon = IconThemeLookup.find(iconRoots, theme, name, size, scale);
            return icon == null ? null : ParcelFileDescriptor.open(icon, ParcelFileDescriptor.MODE_READ_ONLY);
        } catch (IOException error) {
            Log.e(TAG, "Icon lookup failed theme=" + theme + " path=" + uri, error);
            throw new FileNotFoundException("Icon lookup failed for theme " + theme + ": " + error.getMessage());
        } catch (Exception error) {
            Log.e(TAG, "Cannot inspect installed icon theme=" + theme + " path=" + uri, error);
            throw new FileNotFoundException("Cannot inspect icon theme " + theme + ": " + error.getMessage());
        }
    }

    private synchronized Map<String, List<File>> installedRoots() throws Exception {
        Map<String, List<File>> result = new HashMap<>();
        PackageManager pm = getContext().getPackageManager();
        List<PackageInfo> packages = pm.getInstalledPackages(PackageManager.GET_META_DATA);
        Map<String, PackageInfo> byRef = new HashMap<>();
        for (PackageInfo info : packages) {
            if (info.applicationInfo == null || info.applicationInfo.metaData == null) continue;
            String ref = info.applicationInfo.metaData.getString(REF_META);
            if (ref != null) byRef.put(ref, info);
        }
        for (PackageInfo info : packages) {
            if (info.applicationInfo == null || info.applicationInfo.metaData == null) continue;
            String ref = info.applicationInfo.metaData.getString(REF_META);
            if (ref == null || !ref.startsWith("runtime/org.freedesktop.Platform.Icontheme.")) continue;
            String[] pieces = ref.split("/");
            if (pieces.length != 4 || !pieces[2].equals("x86_64")) continue;
            String theme = pieces[1].substring("org.freedesktop.Platform.Icontheme.".length());
            List<File> iconRoots = roots.get(theme);
            if (iconRoots == null) {
                iconRoots = new java.util.ArrayList<>();
                File extensionRoot = packageIconRoot(info, pieces[3]);
                if (extensionRoot == null || !extensionRoot.isDirectory()) continue;
                iconRoots.add(extensionRoot);
                String parentRef = "runtime/org.freedesktop.Platform/x86_64/" + pieces[3];
                PackageInfo parent = byRef.get(parentRef);
                if (parent != null) {
                    File runtimeRoot = packageIconRoot(parent, pieces[3]);
                    if (runtimeRoot != null && runtimeRoot.isDirectory()) iconRoots.add(runtimeRoot);
                }
                roots.put(theme, iconRoots);
            }
            result.put(theme, iconRoots);
        }
        return result;
    }

    private File packageIconRoot(PackageInfo info, String branch) throws Exception {
        File root = packageRoots.get(info.packageName);
        if (root != null) return root;
        IBinder token = new Binder();
        String mounted = ErofsLaunchBridge.mount(info, token);
        root = new File(new File(mounted, branch), "share/icons").getCanonicalFile();
        packageRoots.put(info.packageName, root);
        mountTokens.put(info.packageName, token);
        Log.i(TAG, "Mounted icon files ref=" + info.applicationInfo.metaData.getString(REF_META)
                + " package=" + info.packageName + " root=" + root);
        return root;
    }

    /** Active icon theme: one stored value shared by the tray, Settings and (later) the portal. */
    static final Uri ACTIVE_URI = Uri.parse("content://org.matonos.linux.icons/active");
    private static final String PREFS = "icon_themes", ACTIVE = "active";
    private static final String READ = "org.matonos.permission.READ_ICON_THEMES";
    private static final String WRITE = "org.matonos.permission.SET_ICON_THEME";

    @Override public android.os.Bundle call(String method, String arg, android.os.Bundle extras) {
        android.content.SharedPreferences prefs = getContext().getSharedPreferences(PREFS, android.content.Context.MODE_PRIVATE);
        if ("getActiveTheme".equals(method)) {
            getContext().enforceCallingPermission(READ, "getActiveTheme needs " + READ);
            android.os.Bundle out = new android.os.Bundle();
            out.putString("theme", prefs.getString(ACTIVE, null));
            return out;
        }
        if ("setActiveTheme".equals(method)) {
            getContext().enforceCallingPermission(WRITE, "setActiveTheme needs " + WRITE);
            String theme = arg == null || arg.isEmpty() ? null : arg;
            if (theme != null && (theme.contains("/") || theme.startsWith(".")))
                throw new IllegalArgumentException("Invalid icon theme id: " + theme);
            prefs.edit().putString(ACTIVE, theme).apply();
            Log.i(TAG, "Active icon theme set to " + theme + " by uid=" + Binder.getCallingUid());
            getContext().getContentResolver().notifyChange(ACTIVE_URI, null);
            return null;
        }
        throw new IllegalArgumentException("Unknown icon theme method: " + method);
    }

    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException("read-only"); }
    @Override public int delete(Uri uri, String selection, String[] selectionArgs) { throw new UnsupportedOperationException("read-only"); }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) { throw new UnsupportedOperationException("read-only"); }

    private static final class ErofsLaunchBridge {
        static String mount(PackageInfo info, IBinder token) throws Exception {
            if (info.splitNames == null || info.applicationInfo.splitSourceDirs == null)
                throw new IOException("Icon extension has no split image package=" + info.packageName);
            IBinder binder = (IBinder) Class.forName("android.os.ServiceManager").getMethod("getService", String.class).invoke(null, "matonos.erofs");
            IErofsMounter mounter = IErofsMounter.Stub.asInterface(binder);
            if (mounter == null) throw new IOException("matonos-erofsd is unavailable for package=" + info.packageName);
            String image = null;
            for (int i = 0; i < info.splitNames.length && i < info.applicationInfo.splitSourceDirs.length; i++) {
                if (info.splitNames[i].matches("erofs_[0-9a-f]{64}")) {
                    if (image != null) throw new IOException("Multiple EROFS images package=" + info.packageName);
                    image = info.applicationInfo.splitSourceDirs[i];
                }
            }
            if (image == null) throw new IOException("Icon extension package has no EROFS image: " + info.packageName);
            String path = mounter.mountSplit(image, "assets/matonos/deployment.erofs", token);
            if (path == null || !path.startsWith("/mnt/matonos/")) throw new IOException("Invalid EROFS mount for package=" + info.packageName);
            return path;
        }
    }
}
