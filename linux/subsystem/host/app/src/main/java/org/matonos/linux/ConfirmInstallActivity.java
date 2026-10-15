package org.matonos.linux;

import android.app.Activity;
import android.app.AlertDialog;
import android.os.Bundle;

/** Host-owned confirmation; exported callers cannot supply the displayed caller identity. */
public final class ConfirmInstallActivity extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
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
        new AlertDialog.Builder(this).setTitle("Allow Linux app store request?")
                .setMessage(caller+" wants to "+action+" "+item+".")
                .setNegativeButton("Decline",(dialog,which)->finishRequest(false))
                .setPositiveButton("Accept",(dialog,which)->finishRequest(true))
                .setOnCancelListener(dialog->finishRequest(false)).show();
    }

    private void finishRequest(boolean accepted) {
        String id=getIntent().getStringExtra("operationId");
        LinuxAppStoreService.confirmPending(id,accepted);
        finish();
    }
}
