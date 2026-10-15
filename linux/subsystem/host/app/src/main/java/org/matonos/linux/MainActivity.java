package org.matonos.linux;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

/** Runtime information and a compositor demo owned only by this app. */
public final class MainActivity extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);layout.setPadding(24,24,24,24);
        TextView label = new TextView(this);
        label.setText("Launch Linux applications from their own app icons.");
        layout.addView(label);
        Button demo = new Button(this);demo.setText("Open test window");
        demo.setOnClickListener(v -> startActivity(new Intent(this, WindowActivity.class)
                .putExtra(WindowActivity.EXTRA_START_DEMO, true)));
        layout.addView(demo);setContentView(layout);
    }
}
