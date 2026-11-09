package org.matonos.linux;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;

import org.matonos.linux.runtime.NativeBroker;

import java.util.ArrayList;
import java.util.List;

/** Discovers Android D-Bus providers and hands each a direct seqpacket channel. */
final class DBusBridges implements AutoCloseable {
    private static final String TAG = "MatonDBusBridge";
    private static final String ACTION = "org.matonos.dbus.SERVICE";
    private final Context context;
    private final List<ServiceConnection> bindings = new ArrayList<>();
    private boolean closed;

    DBusBridges(Context context) {
        this.context = context.getApplicationContext();
        discover();
    }

    private void discover() {
        PackageManager pm = context.getPackageManager();
        Intent query = new Intent(ACTION);
        for (ResolveInfo info : pm.queryIntentServices(query, PackageManager.GET_META_DATA)) {
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
            ServiceConnection connection = new ProviderConnection(pkg, names);
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
        final String[] names;
        ProviderConnection(String provider, String[] names) { this.provider=provider;this.names=names; }
        @Override public void onServiceConnected(ComponentName component, IBinder binder) {
            if (closed) return;
            ParcelFileDescriptor providerFd = null;
            ParcelFileDescriptor brokerFd = null;
            FileDescriptor[] fds = null;
            try {
                fds = Os.socketpair(OsConstants.AF_UNIX, OsConstants.SOCK_SEQPACKET, 0);
                providerFd = ParcelFileDescriptor.dup(fds[0]);
                brokerFd = ParcelFileDescriptor.dup(fds[1]);
                Os.close(fds[0]); fds[0] = null;
                Os.close(fds[1]); fds[1] = null;
                IDBusService service = IDBusService.Stub.asInterface(binder);
                service.attach(providerFd);
                providerFd = null; // The provider owns its parcel descriptor.
                NativeBroker.nativeAddBridge(brokerFd.detachFd(), names);
                brokerFd.close();
                brokerFd = null;
            } catch (Exception e) {
                Log.e(TAG, "provider " + provider + " bridge attach failed for " + String.join(",", names), e);
            } finally {
                if (providerFd != null) try { providerFd.close(); } catch (Exception e) { Log.e(TAG, "provider " + provider + " provider fd close failed", e); }
                if (brokerFd != null) try { brokerFd.close(); } catch (Exception e) { Log.e(TAG, "provider " + provider + " broker fd close failed", e); }
                if (fds != null) for (FileDescriptor fd : fds) if (fd != null) try { Os.close(fd); } catch (Exception e) { Log.e(TAG, "provider " + provider + " socketpair fd close failed", e); }
            }
        }
        @Override public void onServiceDisconnected(ComponentName component) { /* Broker observes EOF. */ }
    }

    @Override public void close() {
        closed=true;
        for(ServiceConnection binding:bindings)try{context.unbindService(binding);}catch(RuntimeException e){Log.e(TAG,"provider binding unbind failed",e);}
        bindings.clear();
    }
}
