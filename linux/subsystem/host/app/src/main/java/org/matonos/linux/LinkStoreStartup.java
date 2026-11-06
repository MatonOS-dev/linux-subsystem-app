package org.matonos.linux;

import android.app.Application;
import android.content.Context;
import android.util.Log;
import java.io.File;
import java.util.HashSet;
import java.util.Set;
import org.matonos.linuxhost.stubgen.PlaceholderCleanup;

/** Shared by the subsystem Application and the generated stub's host loader. */
public final class LinkStoreStartup extends Application {
    private static final Set<String> started=new HashSet<>();
    public static void start(Context context) {
        startRoot(context.getDataDir());
        startRoot(context.createDeviceProtectedStorageContext().getDataDir());
    }
    private static synchronized void startRoot(File root) {
        if(!started.add(root.getPath()))return;
        if(!new File(root,".v3-inodes").exists())return;
        new Thread(() -> {
            try { PlaceholderCleanup.sweep(root); }
            catch(Exception | LinkageError e) { Log.w("MatonLinkGC","Hardlink startup sweep failed",e); }
        },"MatonLinkGC").start();
    }
    @Override public void onCreate() { super.onCreate(); start(this); }
}
