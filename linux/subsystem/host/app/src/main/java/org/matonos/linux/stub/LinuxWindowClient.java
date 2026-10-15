package org.matonos.linux.stub;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Binder;
import android.os.IBinder;
import org.matonos.systembridge.ILinuxWindowBridge;

/** Public Android APIs + self-only bridge AIDL. No framework reflection in the stub. */
final class LinuxWindowClient implements ServiceConnection {
    private final Activity activity;
    private final String ref;
    private final Runnable ready;
    private final IBinder lifetime = new Binder();
    private ILinuxWindowBridge bridge;
    private boolean bound, closed;
    private int retries;
    LinuxWindowClient(Activity activity, String ref, Runnable ready) {
        this.activity=activity; this.ref=ref; this.ready=ready;
        bind();
    }
    private void bind() {
        if (closed || bound) return;
        try {
            bound=activity.bindService(new Intent().setComponent(new ComponentName(
                    "org.matonos.systembridge", "org.matonos.systembridge.SystemBridgeService$LinuxWindowService")),
                    this, Context.BIND_AUTO_CREATE);
            android.util.Log.i("MatonLinuxWindow", "bind task=" + activity.getTaskId() + " accepted=" + bound);
            if (!bound) retry();
        } catch (SecurityException error) { log(error); }
    }
    @Override public void onServiceConnected(ComponentName name, IBinder service) {
        bridge=ILinuxWindowBridge.Stub.asInterface(service); android.util.Log.i("MatonLinuxWindow", "connected task=" + activity.getTaskId()); ready.run();
    }
    @Override public void onServiceDisconnected(ComponentName name) { bridge=null; }
    @Override public void onNullBinding(ComponentName name) { retry(); }
    @Override public void onBindingDied(ComponentName name) { retry(); }
    private void retry() {
        bridge=null;
        if (bound) activity.unbindService(this);
        bound=false;
        if (!closed && retries++ < 5) activity.getWindow().getDecorView().postDelayed(this::bind, 500);
    }
    boolean caption(boolean custom) {
        if (bridge == null) { android.util.Log.w("MatonLinuxWindow", "request denied task=" + activity.getTaskId() + " reason=not_connected"); return false; }
        try { boolean accepted = bridge.setClientDecorated(ref, activity.getTaskId(), custom); android.util.Log.i("MatonLinuxWindow", "caption decorated=" + custom + " task=" + activity.getTaskId() + " accepted=" + accepted); return accepted; }
        catch (Exception error) { log(error); return false; }
    }
    boolean begin(int edges) {
        if (bridge == null) { android.util.Log.w("MatonLinuxWindow", "request denied task=" + activity.getTaskId() + " reason=not_connected"); return false; }
        try { boolean accepted = bridge.beginGeometry(ref, activity.getTaskId(), edges, lifetime); android.util.Log.i("MatonLinuxWindow", "begin edges=" + edges + " task=" + activity.getTaskId() + " accepted=" + accepted); return accepted; }
        catch (Exception error) { log(error); return false; }
    }
    boolean update(float dx, float dy) {
        if (bridge == null) { android.util.Log.w("MatonLinuxWindow", "request denied task=" + activity.getTaskId() + " reason=not_connected"); return false; }
        try { boolean accepted = bridge.updateGeometry(ref, activity.getTaskId(), dx, dy); android.util.Log.i("MatonLinuxWindow", "update dx=" + dx + " dy=" + dy + " task=" + activity.getTaskId() + " accepted=" + accepted); return accepted; }
        catch (Exception error) { log(error); return false; }
    }
    void end() {
        if (bridge != null) try { bridge.endGeometry(ref, activity.getTaskId()); android.util.Log.i("MatonLinuxWindow", "end task=" + activity.getTaskId()); }
        catch (Exception error) { log(error); }
    }
    void close() {
        end(); closed=true; bridge=null;
        if (bound) activity.unbindService(this);
        bound=false;
    }
    private static void log(Exception error) { android.util.Log.w("MatonLinuxWindow", "Self-task bridge operation unavailable", error); }
}
