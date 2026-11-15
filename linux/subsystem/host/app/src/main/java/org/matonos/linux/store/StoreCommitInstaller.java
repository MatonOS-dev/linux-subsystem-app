package org.matonos.linux.store;

import android.os.*;
import android.util.Log;
import java.io.*;
import java.util.*;
import java.util.concurrent.*;
import org.matonos.store.source.*;

/** Read-only descriptors live until the store reports the final session result. */
public class StoreCommitInstaller implements CommitChain.Sink {
    private final String operation;
    private final IInstallCallback callback;
    private final Map<String,File> artifacts;
    public StoreCommitInstaller(String operation,IInstallCallback callback,Map<String,File> artifacts) {
        this.operation=operation;this.callback=callback;this.artifacts=artifacts;
    }
    @Override public void commit(CommitChain.Package item) throws Exception {
        if(item.names.length==0&&item.remove.length==0) {
            Log.i("FlatpakSource","Package already current op="+operation+" package="+item.name);return;
        }
        CompletableFuture<Void> completed=new CompletableFuture<>();IBinder binder=callback.asBinder();
        IBinder.DeathRecipient death=()->completed.completeExceptionally(new IOException("Store died during commit package="+item.name));
        binder.linkToDeath(death,0);
        List<ParcelFileDescriptor> opened=new ArrayList<>();
        try {
            for(String name:item.names)opened.add(ParcelFileDescriptor.open(
                    org.matonos.linux.stub.PackInstaller.validateArtifact(artifacts.get(item.name),name),ParcelFileDescriptor.MODE_READ_ONLY));
            callback.commit(operation,item.name,item.names,opened.toArray(new ParcelFileDescriptor[0]),item.remove,
                    new ICommitResult.Stub(){@Override public void result(boolean ok,String error){
                        if(ok)completed.complete(null);else completed.completeExceptionally(new IOException("Store commit failed package="+item.name+": "+error));
                    }});
            completed.get();
        }catch(Exception error){Log.e("FlatpakSource","commit op="+operation+" package="+item.name+" failed",error);throw error;}
        finally {
            binder.unlinkToDeath(death,0);
            for(ParcelFileDescriptor fd:opened) {
                Log.i("FlatpakSource","close fd="+fd.getFd()+" at StoreCommitInstaller.commit package="+item.name);
                try{fd.close();}catch(IOException error){Log.e("FlatpakSource","descriptor close package="+item.name+" failed",error);}
            }
        }
    }
    @Override public void done() throws Exception {callback.done(operation);}
    @Override public void failed(Exception error) throws Exception {Log.e("FlatpakSource","install op="+operation+" failed",error);callback.failed(operation,error.toString());}
}
