package org.matonos.linux;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.IBinder;
import android.util.Log;
import android.widget.TextView;

/** adb-only trigger for the Chrome extra-data install smoke path on userdebug images. */
public final class DebugExtraInstallActivity extends Activity {
    public static final String ACTION = "org.matonos.linux.DEBUG_INSTALL_CHROME";
    private static final String TAG = "MatonExtraInstall";
    private ServiceConnection connection;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        TextView status = new TextView(this); status.setText("Starting Chrome extra-data install");
        status.setGravity(android.view.Gravity.CENTER); setContentView(status);
        if (!ACTION.equals(getIntent().getAction())
                || "user".equals(android.os.Build.TYPE)) {
            Log.e(TAG, "Debug Chrome install is only available on userdebug/eng images"); finish(); return;
        }
        connection = new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName name, IBinder binder) {
                ILinuxAppStore store = ILinuxAppStore.Stub.asInterface(binder);
                String id = "debug-chrome-" + android.os.Process.myPid();
                try {
                    store.install("app/com.google.Chrome/x86_64/stable", "flathub", id,
                            new ILinuxAppStoreCallback.Stub() {
                                @Override public void onProgress(String operationId,String phase,int percent,long done,long total) {
                                    Log.i(TAG, "phase="+phase+" percent="+percent+" bytes="+done+"/"+total);
                                }
                                @Override public void onResult(String operationId,boolean ok,String error) {
                                    Log.i(TAG, "Chrome extra-data install "+(ok?"succeeded":"failed")+(error==null?"":": "+error));
                                    runOnUiThread(()->{status.setText(ok?"Chrome install succeeded":"Chrome install failed: "+error);});
                                }
                            });
                } catch (Exception error) { Log.e(TAG, "Could not start Chrome install", error); finish(); }
            }
            @Override public void onServiceDisconnected(ComponentName name) { Log.e(TAG,"Store service disconnected"); }
        };
        if (!bindService(new Intent(this,LinuxAppStoreService.class),connection,Context.BIND_AUTO_CREATE)) {
            Log.e(TAG,"Could not bind store service"); finish();
        }
    }

    @Override protected void onDestroy() {
        if(connection!=null)try{unbindService(connection);}catch(Exception ignored){}
        super.onDestroy();
    }
}
