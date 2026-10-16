package org.matonos.linux;

import android.app.Activity;
import android.app.AlertDialog;
import android.os.Bundle;

/** Host-owned confirmation; exported callers cannot supply the displayed caller identity. */
public final class ConfirmInstallActivity extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setHideOverlayWindows(true);
        String choice=getIntent().getStringExtra("providerChoice");
        if(choice!=null){try{ProviderChoice.show(this,choice);}catch(Exception error){finish();}return;}
        String kind=getIntent().getStringExtra("kind");
        String ref=getIntent().getStringExtra("ref");
        String remote=getIntent().getStringExtra("remote");
        String name=getIntent().getStringExtra("name");
        String caller=getIntent().getStringExtra("caller");
        String displayRef=ref;
        if(displayRef!=null && displayRef.startsWith("app/")) {
            String[] parts=displayRef.split("/");
            if(parts.length>1) displayRef=parts[1]+" ("+ref+")";
        }
        String item=(displayRef!=null?displayRef:(name==null?"remote":name))+(remote==null?"":" from "+remote);
        String action=(kind==null?"change":kind).replace("Remote"," remote");
        AlertDialog dialog=new AlertDialog.Builder(this).setTitle("Allow Linux app store request?")
                .setMessage(caller+" wants to "+action+" "+item+".")
                .setNegativeButton("Decline",(clicked,which)->finishRequest(false))
                .setPositiveButton("Accept",(clicked,which)->finishRequest(true))
                .setOnCancelListener(cancelled->finishRequest(false)).create();
        dialog.setOnShowListener(ignored->{
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setFilterTouchesWhenObscured(true);
            dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setFilterTouchesWhenObscured(true);
            dialog.getWindow().getDecorView().setFilterTouchesWhenObscured(true);
        });
        dialog.show();
    }

    private void finishRequest(boolean accepted) {
        String id=getIntent().getStringExtra("operationId");
        LinuxAppStoreService.confirmPending(getIntent().getIntExtra("callerUid",-1),id,accepted);
        finish();
    }
}
