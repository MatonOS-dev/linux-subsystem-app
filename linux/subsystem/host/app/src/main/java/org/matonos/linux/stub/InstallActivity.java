package org.matonos.linux.stub;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.Log;
import org.matonos.linux.ILinuxAppStore;
import org.matonos.linux.ILinuxAppStoreCallback;

/** Compatibility action for an already installed, device-signed stub. New
 * installs are owned by the bridge and create this APK after deployment.
 * linuxd authenticates this package and uses FPINSTALL for network staging. */
public class InstallActivity extends Activity {
    private static final String TAG = "MatonStubInstall";
    /** Optional operation ID from the store, so it can follow progress. */
    public static final String EXTRA_OPERATION_ID = "org.matonos.linuxhost.OPERATION_ID";

    protected boolean update() { return false; }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        final String ref;
        try {
            android.content.pm.ActivityInfo info=getPackageManager().getActivityInfo(
                    new android.content.ComponentName(getPackageName(),org.matonos.linuxhost.stubgen.StubGenerator.HOST_ACTIVITY),android.content.pm.PackageManager.GET_META_DATA);
            ref=info.metaData.getString(org.matonos.linuxhost.stubgen.StubGenerator.REF_META);
            if(!DeployManifest.validRef(ref))throw new IllegalArgumentException("Invalid stub ref");
        } catch(Exception error){Log.e(TAG,"Cannot read installed stub identity",error);finish();return;}
        String operation = getIntent().getStringExtra(EXTRA_OPERATION_ID);
        final String operationId = operation == null ? "" : operation;
        Intent store = new Intent().setClassName("org.matonos.linux", "org.matonos.linux.LinuxAppStoreService");
        bindService(store, new android.content.ServiceConnection() {
            @Override public void onServiceConnected(android.content.ComponentName name, IBinder service) {
                try {
                    ILinuxAppStore api=ILinuxAppStore.Stub.asInterface(service);
                    ILinuxAppStoreCallback callback=new ILinuxAppStoreCallback.Stub() {
                        @Override public void onProgress(String id,String phase,int percent,long done,long total) { }
                        @Override public void onResult(String id,boolean ok,String error) {
                            Log.i(TAG, (update()?"update":"install")+" me: "+(ok?"complete":error));
                            unbindService(thisConnectionHolder[0]); finish();
                        }
                    };
                    if(update())api.update(ref,null,null,operationId.isEmpty()?"self-update":operationId,callback);
                    else api.install(ref, "flathub", null, null, operationId.isEmpty()?"self-install":operationId,callback);
                } catch(Exception error) { Log.e(TAG,"Cannot request Flatpak install",error); finish(); }
            }
            final android.content.ServiceConnection[] thisConnectionHolder={this};
            @Override public void onServiceDisconnected(android.content.ComponentName name) { finish(); }
        }, BIND_AUTO_CREATE);
    }
}
