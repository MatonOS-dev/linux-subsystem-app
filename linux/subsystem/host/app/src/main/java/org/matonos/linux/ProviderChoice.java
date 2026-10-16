package org.matonos.linux;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import java.io.IOException;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.BooleanSupplier;
import org.matonos.linuxhost.stubgen.RemoteOrigin;

/** Internal rendezvous between the download worker and subsystem-owned confirmation UI. */
public final class ProviderChoice {
    private static final class Request {
        final String ref;final List<String> origins;final CompletableFuture<String> answer=new CompletableFuture<>();
        Request(String ref,List<String> origins){this.ref=ref;this.origins=List.copyOf(origins);}
    }
    private static final Map<String,Request> pending=new ConcurrentHashMap<>();
    public static String choose(Context context,String ref,List<String> origins,BooleanSupplier cancelled)throws Exception {
        String token=UUID.randomUUID().toString();Request request=new Request(ref,origins);pending.put(token,request);
        try {
            context.startActivity(new Intent(context,ConfirmInstallActivity.class).putExtra("providerChoice",token).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            long deadline=System.nanoTime()+TimeUnit.MINUTES.toNanos(15);
            while(!cancelled.getAsBoolean()&&System.nanoTime()<deadline)try{return request.answer.get(1,TimeUnit.SECONDS);}catch(TimeoutException waiting){}
            throw new IOException("Dependency origin selection cancelled or expired");
        }finally{pending.remove(token,request);}
    }
    static void show(Activity activity,String token)throws Exception {
        Request request=pending.get(token);if(request==null){activity.finish();return;}
        String[] labels=new String[request.origins.size()];
        for(int i=0;i<labels.length;i++)labels[i]=RemoteOrigin.parse(request.origins.get(i)).display();
        int[] selected={-1};
        AlertDialog dialog=new AlertDialog.Builder(activity).setTitle("Choose dependency remote")
                .setSingleChoiceItems(labels,-1,(clicked,index)->{selected[0]=index;((AlertDialog)clicked).getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true);})
                .setMessage(null)
                .setPositiveButton("Install",(clicked,which)->{if(selected[0]>=0)request.answer.complete(request.origins.get(selected[0]));activity.finish();})
                .setNegativeButton("Cancel",(clicked,which)->{request.answer.complete(null);activity.finish();})
                .setOnCancelListener(clicked->{request.answer.complete(null);activity.finish();}).create();
        dialog.setTitle("Choose remote for "+request.ref);
        dialog.setOnShowListener(ignored->{
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(false);
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setFilterTouchesWhenObscured(true);
            dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setFilterTouchesWhenObscured(true);
            dialog.getListView().setFilterTouchesWhenObscured(true);
        });dialog.show();
    }
    private ProviderChoice(){}
}
