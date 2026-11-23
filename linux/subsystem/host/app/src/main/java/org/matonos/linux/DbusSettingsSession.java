package org.matonos.linux;

import android.content.Context;
import android.content.res.Configuration;
import android.database.ContentObserver;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.RemoteException;
import android.util.Log;
import android.util.TypedValue;
import android.view.Display;
import android.view.Gravity;
import android.view.SurfaceControlViewHost;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import org.matonos.appsettings.IEmbeddedSettingsHost;
import org.json.JSONArray;
import org.json.JSONObject;

final class DbusSettingsSession {
    private static final String TAG = "MatonDBusSettings";
    private final Context context;
    private final IEmbeddedSettingsHost host;
    private final SurfaceControlViewHost surfaceHost;
    private final int foreground, background, secondary;
    private final LinearLayout rows;
    private final ContentObserver changes;
    private int width, height;
    private boolean released;

    DbusSettingsSession(Context context, Display display, android.os.IBinder token, IEmbeddedSettingsHost host) throws Exception {
        this.context = context;
        this.host = host;
        surfaceHost = new SurfaceControlViewHost(context, display, token);
        boolean dark = (context.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES;
        background = dark ? Color.rgb(28, 28, 30) : Color.rgb(250, 249, 252);
        foreground = dark ? Color.WHITE : Color.rgb(25, 27, 31);
        secondary = dark ? Color.LTGRAY : Color.DKGRAY;
        ScrollView scroll = new ScrollView(context);
        scroll.setBackgroundColor(background);
        rows = new LinearLayout(context);
        rows.setOrientation(LinearLayout.VERTICAL);
        rows.setPadding(dp(20), dp(12), dp(20), dp(20));
        scroll.addView(rows, new ScrollView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        surfaceHost.setView(scroll, 1, 1);
        changes = new ContentObserver(new Handler(context.getMainLooper())) {
            @Override public void onChange(boolean selfChange) { refresh(); }
        };
        context.getContentResolver().registerContentObserver(DBusProviderSettings.URI, false, changes);
        refresh();
        try {
            host.setSurface(surfaceHost.getSurfacePackage());
            host.setPreferredHeight(dp(420));
        } catch (RemoteException error) {
            Log.e(TAG, "surface callback failed", error);
            release();
            throw error;
        }
    }

    void resize(int widthPx, int heightPx) {
        width = Math.max(1, widthPx); height = Math.max(1, heightPx);
        surfaceHost.relayout(width, height);
    }

    void release() {
        if (released) return;
        released = true;
        context.getContentResolver().unregisterContentObserver(changes);
        surfaceHost.release();
    }

    private void refresh() {
        if (released) return;
        try {
            Bundle result = context.getContentResolver().call(DBusProviderSettings.URI, "refreshProviders", null, Bundle.EMPTY);
            String json = result == null ? null : result.getString("providers");
            if (json == null) throw new IllegalStateException("Provider settings returned no provider list");
            render(new JSONArray(json));
        } catch (Exception error) {
            Log.e(TAG, "provider settings refresh failed", error);
            renderError("Could not load D-Bus providers. See system log for details.");
        }
    }

    private void render(JSONArray providers) throws Exception {
        rows.removeAllViews();
        TextView title = text("D-Bus services", 20, foreground);
        title.setPadding(0, 0, 0, dp(6));
        rows.addView(title);
        TextView intro = text("Choose which installed app handles each service name.", 14, secondary);
        intro.setPadding(0, 0, 0, dp(12));
        rows.addView(intro);
        if (providers.length() == 0) {
            rows.addView(text("No D-Bus providers are installed.", 15, secondary));
            return;
        }
        for (int i = 0; i < providers.length(); i++) {
            JSONObject provider = providers.getJSONObject(i);
            String name = provider.getString("name");
            String selected = provider.optString("selected", "");
            JSONArray claims = provider.getJSONArray("claimants");
            LinearLayout group = new LinearLayout(context);
            group.setOrientation(LinearLayout.VERTICAL);
            group.setPadding(0, dp(10), 0, dp(10));
            TextView label = text(name, 16, foreground);
            group.addView(label);
            String[] labels = new String[claims.length()];
            String[] ids = new String[claims.length()];
            int selectedIndex = 0;
            for (int j = 0; j < claims.length(); j++) {
                JSONObject claim = claims.getJSONObject(j);
                ids[j] = claim.getString("id");
                labels[j] = claim.getString("label") + "  ·  " + claim.getString("package");
                if (selected.equals(ids[j])) selectedIndex = j;
            }
            if (claims.length() == 0) continue;
            Spinner choice = new Spinner(context);
            ArrayAdapter<String> adapter = new ArrayAdapter<>(context, android.R.layout.simple_spinner_item, labels);
            adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
            choice.setAdapter(adapter);
            choice.setSelection(selectedIndex, false);
            choice.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
                boolean first = true;
                @Override public void onItemSelected(android.widget.AdapterView<?> parent, View view, int position, long id) {
                    if (first) { first = false; return; }
                    if (position < 0 || position >= ids.length || ids[position].equals(selected)) return;
                    try {
                        Bundle extras = new Bundle(); extras.putString("name", name);
                        context.getContentResolver().call(DBusProviderSettings.URI, "setChoice", ids[position], extras);
                    } catch (Exception error) { Log.e(TAG, "handler selection failed name=" + name + " id=" + ids[position], error); }
                }
                @Override public void onNothingSelected(android.widget.AdapterView<?> parent) { }
            });
            group.addView(choice);
            rows.addView(group, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            View divider = new View(context); divider.setBackgroundColor(darkDivider());
            rows.addView(divider, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1)));
        }
    }

    private int darkDivider() { return foreground & 0x22ffffff; }
    private void renderError(String message) {
        rows.removeAllViews();
        TextView error = text(message, 15, foreground);
        rows.addView(error);
    }
    private TextView text(String value, int size, int color) {
        TextView view = new TextView(context); view.setText(value); view.setTextColor(color);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, size); view.setGravity(Gravity.CENTER_VERTICAL); return view;
    }
    private int dp(int value) { return Math.round(value * context.getResources().getDisplayMetrics().density); }
}
