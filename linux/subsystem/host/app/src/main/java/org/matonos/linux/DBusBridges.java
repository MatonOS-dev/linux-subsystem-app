package org.matonos.linux;

import android.content.ComponentName;
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

/** Discovers Android D-Bus providers and hands each a direct seqpacket channel. */
final class DBusBridges implements AutoCloseable {
    private static final String TAG = "MatonDBusBridge";
    private static final String ACTION = "org.matonos.dbus.SERVICE";
    private final Context context;
    private final List<ServiceConnection> bindings = new ArrayList<>();
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private boolean closed;
    private volatile File appFiles;
    private volatile File runtimeFiles;

    DBusBridges(Context context) {
        this.context = context.getApplicationContext();
        discover();
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

        @Override public ParcelFileDescriptor openIcon(String iconThemePath, String iconName, int sizePx) {
            int uid = Binder.getCallingUid();
            try {
                File home = new File(context.getDataDir().getCanonicalFile(), "home");
                File icon = StubIconLookup.find(appFiles, runtimeFiles, home, iconThemePath, iconName, sizePx);
                if (icon == null) return null;
                return ParcelFileDescriptor.open(icon, ParcelFileDescriptor.MODE_READ_ONLY);
            } catch (Exception error) {
                Log.e(TAG, "stub icon open failed uid=" + uid + " path=" + iconThemePath
                        + " name=" + iconName + " size=" + sizePx, error);
                return null;
            }
        }
    };

    private void discover() {
        PackageManager pm = context.getPackageManager();
        Intent query = new Intent(ACTION);
        List<ResolveInfo> providers = pm.queryIntentServices(query, PackageManager.GET_META_DATA);
        Log.i(TAG, "discovered " + providers.size() + " D-Bus provider(s) for " + ACTION);
        for (ResolveInfo info : providers) {
            if (info.serviceInfo == null) continue;
            String pkg = info.serviceInfo.packageName;
            String raw = info.serviceInfo.metaData == null ? null :
                    info.serviceInfo.metaData.getString("org.matonos.dbus.NAMES");
            if (raw == null || raw.trim().isEmpty()) {
                Log.e(TAG, "provider " + pkg + " has no org.matonos.dbus.NAMES metadata");
                continue;
            }
            String[] names = raw.split(",");
            for (int i=0;i<names.length;i++) names[i]=names[i].trim();
            Intent bind = new Intent(ACTION).setComponent(new ComponentName(pkg, info.serviceInfo.name));
            int uid = info.serviceInfo.applicationInfo == null ? -1 : info.serviceInfo.applicationInfo.uid;
            ProviderConnection connection = new ProviderConnection(pkg, uid, bind, names);
            try {
                if (context.bindService(bind, connection, Context.BIND_AUTO_CREATE)) bindings.add(connection);
                else Log.e(TAG, "provider " + pkg + " bindService returned false");
            } catch (RuntimeException e) {
                Log.e(TAG, "provider " + pkg + " bind failed", e);
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
                if (closed) return;
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
        mainHandler.removeCallbacksAndMessages(null);
        for(ServiceConnection binding:bindings)try{context.unbindService(binding);}catch(RuntimeException e){Log.e(TAG,"provider binding unbind failed",e);}
        bindings.clear();
    }
}
