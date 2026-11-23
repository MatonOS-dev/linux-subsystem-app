package org.matonos.linux;

import android.content.ComponentName;
import android.content.BroadcastReceiver;
import android.content.IntentFilter;
import android.database.ContentObserver;
import android.net.Uri;
import android.os.Bundle;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.os.IBinder;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.Binder;
import android.system.Os;
import android.system.OsConstants;
import java.io.FileDescriptor;
import java.io.File;
import android.util.Log;

import org.matonos.linux.runtime.NativeBroker;
import org.matonos.linux.stub.StubIconLookup;

import java.util.ArrayList;
import java.util.List;
import java.util.LinkedHashMap;
import java.util.Map;
import org.json.JSONArray;
import org.json.JSONObject;

/** Discovers Android D-Bus providers and hands each a direct seqpacket channel. */
final class DBusBridges implements AutoCloseable {
    private static final String TAG = "MatonDBusBridge";
    private static final String ACTION = "org.matonos.dbus.SERVICE";
    private static final Uri SETTINGS_URI = Uri.parse("content://org.matonos.linux.dbus-settings/providers");
    private final Context context;
    private final String appId;
    private final List<ServiceConnection> bindings = new ArrayList<>();
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private boolean closed;
    private volatile File appFiles;
    private volatile File runtimeFiles;
    private final ContentObserver settingsObserver;
    private final BroadcastReceiver packageReceiver;
    private boolean refreshing;

    DBusBridges(Context context) {
        this.context = context.getApplicationContext();
        this.appId = owningAppId(this.context);
        settingsObserver = new ContentObserver(mainHandler) {
            @Override public void onChange(boolean selfChange) { refreshBindings("settings changed"); }
        };
        packageReceiver = new BroadcastReceiver() {
            @Override public void onReceive(Context ignored, Intent intent) {
                mainHandler.post(() -> {
                    try { readSelections(true); }
                    catch (RuntimeException error) { Log.e(TAG, "provider refresh after package event failed action=" + intent.getAction(), error); }
                });
            }
        };
        context.getContentResolver().registerContentObserver(SETTINGS_URI, false, settingsObserver);
        IntentFilter filter = new IntentFilter();
        filter.addAction(Intent.ACTION_PACKAGE_ADDED);
        filter.addAction(Intent.ACTION_PACKAGE_REMOVED);
        filter.addAction(Intent.ACTION_PACKAGE_REPLACED);
        filter.addDataScheme("package");
        context.registerReceiver(packageReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        discover(readSelections(true));
    }

    private static String owningAppId(Context context) {
        try {
            android.content.pm.ApplicationInfo info = context.getPackageManager().getApplicationInfo(
                    context.getPackageName(), PackageManager.GET_META_DATA);
            String ref = info.metaData == null ? null : info.metaData.getString("org.matonos.linuxhost.FLATPAK_REF");
            if (ref != null) {
                String[] parts = ref.split("/");
                if (parts.length == 4 && parts[0].equals("app") && parts[1].matches("[A-Za-z0-9_.-]+"))
                    return parts[1];
            }
            Log.e(TAG, "Cannot map Flatpak /var directories package=" + context.getPackageName()
                    + " ref=" + ref);
        } catch (PackageManager.NameNotFoundException error) {
            Log.e(TAG, "Cannot read owning app metadata package=" + context.getPackageName(), error);
        }
        return null;
    }

    void setIconDirectories(File appFiles, File runtimeFiles) {
        this.appFiles = appFiles;
        this.runtimeFiles = runtimeFiles;
    }

    private final IStubIcons iconCallback = new IStubIcons.Stub() {
        @Override public boolean hasLiveWindow() {
            int uid = Binder.getCallingUid();
            boolean live = org.matonos.linux.stub.StubActivity.hasLiveWindow();
            Log.i(TAG, "live window query uid=" + uid + " package=" + context.getPackageName()
                    + " result=" + live);
            return live;
        }

        @Override public void finishPlaceholderIfNoWindow() {
            int uid = Binder.getCallingUid();
            Log.i(TAG, "finish empty tray placeholder requested uid=" + uid
                    + " package=" + context.getPackageName());
            org.matonos.linux.stub.StubActivity.finishPlaceholdersWithoutWindow();
        }

        @Override public ParcelFileDescriptor openIcon(String iconThemePath, String iconName, int sizePx) {
            int uid = Binder.getCallingUid();
            try {
                File home = new File(context.getDataDir().getCanonicalFile(), "home");
                File icon = StubIconLookup.find(appFiles, runtimeFiles, home, appId,
                        iconThemePath, iconName, sizePx);
                if (icon == null) return null;
                return ParcelFileDescriptor.open(icon, ParcelFileDescriptor.MODE_READ_ONLY);
            } catch (Exception error) {
                Log.e(TAG, "stub icon open failed uid=" + uid + " path=" + iconThemePath
                        + " name=" + iconName + " size=" + sizePx, error);
                return null;
            }
        }
    };

    private Map<String, List<String>> readSelections(boolean refreshProvider) {
        Map<String, List<String>> byComponent = new LinkedHashMap<>();
        try {
            Bundle result = context.getContentResolver().call(SETTINGS_URI,
                    refreshProvider ? "refreshProviders" : "getProviders", null, Bundle.EMPTY);
            String json = result == null ? null : result.getString("providers");
            if (json == null) throw new IllegalStateException("provider settings returned no list");
            JSONArray rows = new JSONArray(json);
            for (int i = 0; i < rows.length(); i++) {
                JSONObject row = rows.getJSONObject(i);
                String name = row.getString("name"), selected = row.optString("selected", "");
                if (selected.isEmpty()) continue;
                List<String> names = byComponent.get(selected);
                if (names == null) { names = new ArrayList<>(); byComponent.put(selected, names); }
                names.add(name);
            }
        } catch (Exception error) {
            Log.e(TAG, "Cannot read subsystem D-Bus provider choices; no providers will be bridged", error);
        }
        return byComponent;
    }

    private void refreshBindings(String reason) {
        if (closed || refreshing) return;
        refreshing = true;
        try {
            Log.i(TAG, "refreshing D-Bus provider bindings reason=" + reason);
            NativeBroker.nativeClearBridges();
            for (ServiceConnection binding : bindings) {
                try { context.unbindService(binding); }
                catch (RuntimeException error) { Log.e(TAG, "provider unbind during refresh failed", error); }
            }
            bindings.clear();
            discover(readSelections(true));
        } finally { refreshing = false; }
    }

    private void discover(Map<String, List<String>> selectedByComponent) {
        PackageManager pm = context.getPackageManager();
        Intent query = new Intent(ACTION);
        List<ResolveInfo> providers = pm.queryIntentServices(query, PackageManager.GET_META_DATA);
        Log.i(TAG, "discovered " + providers.size() + " D-Bus provider(s), selected components=" + selectedByComponent.size());
        for (ResolveInfo info : providers) {
            if (info.serviceInfo == null) continue;
            String pkg = info.serviceInfo.packageName;
            ComponentName component = new ComponentName(pkg, info.serviceInfo.name);
            String id = component.flattenToString();
            List<String> selectedNames = selectedByComponent.get(id);
            if (selectedNames == null || selectedNames.isEmpty()) continue;
            String raw = info.serviceInfo.metaData == null ? null :
                    info.serviceInfo.metaData.getString("org.matonos.dbus.NAMES");
            if (raw == null || raw.trim().isEmpty()) {
                Log.e(TAG, "selected provider " + pkg + " has no org.matonos.dbus.NAMES metadata");
                continue;
            }
            List<String> claimed = new ArrayList<>();
            for (String rawName : raw.split(",")) {
                String name = rawName.trim();
                if (selectedNames.contains(name)) claimed.add(name);
            }
            if (claimed.isEmpty()) {
                Log.e(TAG, "selected provider metadata does not claim selected names package=" + pkg + " component=" + id);
                continue;
            }
            Intent bind = new Intent(ACTION).setComponent(component);
            int uid = info.serviceInfo.applicationInfo == null ? -1 : info.serviceInfo.applicationInfo.uid;
            ProviderConnection connection = new ProviderConnection(pkg, uid, bind, claimed.toArray(new String[0]));
            try {
                if (context.bindService(bind, connection, Context.BIND_AUTO_CREATE)) bindings.add(connection);
                else Log.e(TAG, "selected provider " + pkg + " bindService returned false names=" + String.join(",", claimed));
            } catch (RuntimeException error) {
                Log.e(TAG, "selected provider bind failed package=" + pkg + " names=" + String.join(",", claimed), error);
            }
        }
    }

    private final class ProviderConnection implements ServiceConnection {
        final String provider;
        final int uid;
        final Intent bindIntent;
        final String[] names;
        boolean bound = true;
        boolean everConnected;
        boolean rebindPending;

        ProviderConnection(String provider, int uid, Intent bindIntent, String[] names) {
            this.provider = provider;
            this.uid = uid;
            this.bindIntent = bindIntent;
            this.names = names;
        }

        @Override public void onServiceConnected(ComponentName component, IBinder binder) {
            boolean reconnect;
            synchronized (this) {
                if (closed || !bindings.contains(this)) {
                    Log.i(TAG, "ignoring connection for retired D-Bus provider package=" + provider + " component=" + component);
                    return;
                }
                reconnect = everConnected;
                everConnected = true;
                rebindPending = false;
            }
            Log.i(TAG, "provider service connected package=" + provider + " uid=" + uid
                    + " reconnect=" + reconnect + " component=" + component
                    + " names=" + String.join(",", names));
            ParcelFileDescriptor[] pair = null;
            try {
                FileDescriptor a = new FileDescriptor(), b = new FileDescriptor();
                Os.socketpair(OsConstants.AF_UNIX, OsConstants.SOCK_SEQPACKET | OsConstants.SOCK_CLOEXEC, 0, a, b);
                try { pair = new ParcelFileDescriptor[]{ParcelFileDescriptor.dup(a), ParcelFileDescriptor.dup(b)}; }
                finally { Os.close(a); Os.close(b); }
                IDBusService.Stub.asInterface(binder).attachWithIcons(pair[0], iconCallback); // Binder duplicates the descriptors.
                NativeBroker.nativeAddBridge(pair[1].detachFd(), names);
                Log.i(TAG, "provider bridge attached package=" + provider + " uid=" + uid
                        + " reconnect=" + reconnect + " names=" + String.join(",", names));
            } catch (Exception e) {
                Log.e(TAG, "provider " + provider + " uid=" + uid + " bridge attach failed for "
                        + String.join(",", names), e);
            } finally {
                if (pair != null) for (ParcelFileDescriptor fd : pair) try { fd.close(); } catch (Exception e) { Log.e(TAG, "provider " + provider + " uid=" + uid + " socketpair fd close failed", e); }
            }
        }

        @Override public void onServiceDisconnected(ComponentName component) {
            Log.e(TAG, "provider service disconnected; broker observes dead socket package=" + provider
                    + " uid=" + uid + " component=" + component + " names=" + String.join(",", names));
        }

        @Override public void onBindingDied(ComponentName component) {
            Log.e(TAG, "provider binding died; scheduling unbind and rebind package=" + provider
                    + " uid=" + uid + " component=" + component + " names=" + String.join(",", names));
            scheduleRebind();
        }

        @Override public void onNullBinding(ComponentName component) {
            Log.e(TAG, "provider returned null binding; scheduling unbind and rebind package=" + provider
                    + " uid=" + uid + " component=" + component + " names=" + String.join(",", names));
            scheduleRebind();
        }

        private void scheduleRebind() {
            synchronized (this) {
                if (closed || rebindPending) return;
                rebindPending = true;
            }
            mainHandler.postDelayed(this::unbindAndRebind, 1000);
        }

        private void unbindAndRebind() {
            synchronized (this) {
                if (closed) return;
                rebindPending = false;
                if (bound) {
                    try {
                        context.unbindService(this);
                    } catch (RuntimeException error) {
                        Log.e(TAG, "provider unbind before rebind failed package=" + provider
                                + " uid=" + uid, error);
                    }
                    bound = false;
                }
                try {
                    bound = context.bindService(bindIntent, this, Context.BIND_AUTO_CREATE);
                    if (bound) {
                        Log.i(TAG, "provider rebound package=" + provider + " uid=" + uid
                                + " names=" + String.join(",", names));
                    } else {
                        Log.e(TAG, "provider rebind returned false package=" + provider + " uid=" + uid);
                        scheduleRebind();
                    }
                } catch (RuntimeException error) {
                    Log.e(TAG, "provider rebind failed package=" + provider + " uid=" + uid, error);
                    scheduleRebind();
                }
            }
        }
    }

    @Override public void close() {
        closed=true;
        context.getContentResolver().unregisterContentObserver(settingsObserver);
        try { context.unregisterReceiver(packageReceiver); }
        catch (RuntimeException error) { Log.e(TAG, "package receiver unregister failed", error); }
        NativeBroker.nativeClearBridges();
        mainHandler.removeCallbacksAndMessages(null);
        for(ServiceConnection binding:bindings)try{context.unbindService(binding);}catch(RuntimeException e){Log.e(TAG,"provider binding unbind failed",e);}
        bindings.clear();
    }
}
