package org.matonos.linux;

import android.app.Activity;
import android.os.Bundle;

/** Host-owned confirmation; exported callers cannot supply the displayed caller identity. */
public final class ProviderChoiceActivity extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setHideOverlayWindows(true);
        String choice=getIntent().getStringExtra("providerChoice");
        if(choice!=null){try{ProviderChoice.show(this,choice);}catch(Exception error){finish();}return;}
        finish();
    }
}
