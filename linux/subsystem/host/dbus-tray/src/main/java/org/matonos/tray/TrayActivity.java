package org.matonos.tray;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Typeface;
import android.graphics.Bitmap;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;

/** Plain debug listing for StatusNotifierItems. */
public final class TrayActivity extends Activity {
  private LinearLayout rows;
  private final BroadcastReceiver updates =
      new BroadcastReceiver() {
        @Override
        public void onReceive(Context c, Intent i) {
          render();
        }
      };

  @Override
  protected void onCreate(Bundle state) {
    super.onCreate(state);
    ScrollView scroll = new ScrollView(this);
    rows = new LinearLayout(this);
    rows.setOrientation(LinearLayout.VERTICAL);
    rows.setPadding(12, 12, 12, 12);
    scroll.addView(rows);
    // targetSdk 35+ is edge-to-edge: keep rows clear of the action bar and system bars.
    scroll.setOnApplyWindowInsetsListener(
        (view, insets) -> {
          android.graphics.Insets bars =
              insets.getInsets(
                  android.view.WindowInsets.Type.systemBars()
                      | android.view.WindowInsets.Type.displayCutout());
          view.setPadding(bars.left, bars.top, bars.right, bars.bottom);
          return insets;
        });
    setContentView(scroll);
    registerReceiver(
        updates, new IntentFilter(TrayService.ACTION_ITEMS), Context.RECEIVER_NOT_EXPORTED);
    render();
  }

  @Override
  protected void onDestroy() {
    unregisterReceiver(updates);
    super.onDestroy();
  }

  private void render() {
    if (rows == null) return;
    rows.removeAllViews();
    List<TrayItem> items = new ArrayList<>(TrayService.ITEMS.values());
    if (items.isEmpty()) {
      TextView empty = new TextView(this);
      empty.setText("No StatusNotifierItems registered");
      empty.setPadding(16, 16, 16, 16);
      rows.addView(empty);
      return;
    }
    for (TrayItem item : items) {
      LinearLayout row = new LinearLayout(this);
      row.setGravity(Gravity.CENTER_VERTICAL);
      row.setPadding(8, 8, 8, 8);
      ImageView icon = new ImageView(this);
      IconThemeResolver.Result themed = IconThemeResolver.resolve(this, item.iconName,
          Math.round(32 * getResources().getDisplayMetrics().density));
      Bitmap displayed = themed.bitmap != null ? themed.bitmap : item.icon;
      if (displayed != null) {
        icon.setImageBitmap(displayed);
        if (themed.bitmap != null && themed.symbolic) icon.setColorFilter(android.graphics.Color.WHITE);
      }
      else {
        icon.setContentDescription(item.iconName);
        icon.setScaleType(ImageView.ScaleType.CENTER);
      }
      row.addView(icon, new LinearLayout.LayoutParams(48, 48));
      if (displayed == null && !item.iconName.isEmpty()) {
        TextView iconName = new TextView(this);
        iconName.setText(item.iconName);
        iconName.setTextSize(10);
        row.addView(iconName);
      }
      TextView text = new TextView(this);
      text.setTypeface(null, Typeface.BOLD);
      text.setText(item.title.isEmpty() ? item.id : item.title);
      text.setPadding(12, 0, 0, 0);
      row.addView(
          text, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
      TextView status = new TextView(this);
      status.setText(item.status);
      row.addView(status);
      row.setOnClickListener(v -> item.activateFromWidget(this));
      row.setOnLongClickListener(
          v -> {
            startActivity(
                new android.content.Intent(this, TrayMenuActivity.class)
                    .putExtra(TrayMenuActivity.EXTRA_ITEM_KEY, TrayService.key(item.attachId, item.service))
                    .putExtra(TrayMenuActivity.EXTRA_OPEN_MENU, true));
            return true;
          });
      rows.addView(row);
      View divider = new View(this);
      divider.setBackgroundColor(0xff888888);
      rows.addView(divider, new LinearLayout.LayoutParams(-1, 1));
    }
  }
}
