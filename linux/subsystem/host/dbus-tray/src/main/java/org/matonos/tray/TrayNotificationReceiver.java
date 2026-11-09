package org.matonos.tray;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

/** Routes tray notification actions without showing an intermediate activity. */
public final class TrayNotificationReceiver extends BroadcastReceiver {
  static final String ACTION_ACTIVATE = "org.matonos.tray.NOTIFICATION_ACTIVATE";
  static final String ACTION_MENU = "org.matonos.tray.NOTIFICATION_MENU";

  @Override
  public void onReceive(Context context, Intent intent) {
    String key = intent.getStringExtra(TrayMenuActivity.EXTRA_ITEM_KEY);
    TrayItem item = key == null ? null : TrayService.ITEMS.get(key);
    if (item == null) {
      Log.e(TrayService.TAG, "Notification action item missing package=" + context.getPackageName()
          + " key=" + key + " action=" + intent.getAction());
      return;
    }
    if (ACTION_ACTIVATE.equals(intent.getAction())) {
      item.activate(0, 0);
      return;
    }
    if (ACTION_MENU.equals(intent.getAction())) {
      Intent menu = new Intent(context, TrayMenuActivity.class)
          .putExtra(TrayMenuActivity.EXTRA_ITEM_KEY, key)
          .putExtra(TrayMenuActivity.EXTRA_OPEN_MENU, true)
          .putExtra(TrayMenuActivity.EXTRA_TOP_FALLBACK, true)
          .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
      try {
        context.startActivity(menu);
      } catch (RuntimeException error) {
        Log.e(TrayService.TAG, "Notification menu launch failed package=" + context.getPackageName()
            + " key=" + key, error);
      }
      return;
    }
    Log.e(TrayService.TAG, "Unknown notification action package=" + context.getPackageName()
        + " key=" + key + " action=" + intent.getAction());
  }
}
