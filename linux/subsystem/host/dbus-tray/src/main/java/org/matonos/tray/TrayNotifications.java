package org.matonos.tray;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.drawable.Icon;
import android.net.Uri;
import android.os.Build;
import android.util.Log;
import java.util.HashSet;
import java.util.Set;

/** Posts one ongoing notification for each registered StatusNotifierItem. */
final class TrayNotifications {
  private static final String CHANNEL_ID = "tray_items";
  private static final int NOTIFICATION_ID = 0;
  private static final int REQUEST_ACTIVATE = 1;
  private static final int REQUEST_MENU = 2;
  private static final Set<String> POSTED_KEYS = new HashSet<>();
  private static boolean permissionFailureLogged;

  private TrayNotifications() {}

  static synchronized void refresh(Context context) {
    NotificationManager manager = context.getSystemService(NotificationManager.class);
    if (manager == null) {
      Log.e(TrayService.TAG, "NotificationManager unavailable package=" + context.getPackageName());
      return;
    }
    ensureChannel(manager);
    if (Build.VERSION.SDK_INT >= 33
        && context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
            != PackageManager.PERMISSION_GRANTED) {
      if (!permissionFailureLogged) {
        permissionFailureLogged = true;
        Log.e(TrayService.TAG, "Cannot post tray notifications package=" + context.getPackageName()
            + " reason=POST_NOTIFICATIONS is not granted; pill remains available");
      }
      cancelAll(manager);
      return;
    }

    Set<String> active = new HashSet<>(TrayService.ITEMS.keySet());
    for (String oldKey : new HashSet<>(POSTED_KEYS)) {
      if (!active.contains(oldKey)) manager.cancel(oldKey, NOTIFICATION_ID);
    }
    POSTED_KEYS.retainAll(active);
    for (TrayItem item : TrayService.ITEMS.values()) {
      String key = TrayService.key(item.attachId, item.service);
      try {
        manager.notify(key, NOTIFICATION_ID, build(context, item, key));
        POSTED_KEYS.add(key);
      } catch (RuntimeException error) {
        Log.e(TrayService.TAG, "Tray notification update failed package=" + context.getPackageName()
            + " service=" + item.service + " key=" + key, error);
      }
    }
  }

  static synchronized void cancelAll(Context context) {
    NotificationManager manager = context.getSystemService(NotificationManager.class);
    if (manager != null) cancelAll(manager);
  }

  private static void cancelAll(NotificationManager manager) {
    for (String key : POSTED_KEYS) manager.cancel(key, NOTIFICATION_ID);
    POSTED_KEYS.clear();
  }

  private static void ensureChannel(NotificationManager manager) {
    NotificationChannel channel = new NotificationChannel(
        CHANNEL_ID, "Tray items", NotificationManager.IMPORTANCE_LOW);
    channel.setDescription("Ongoing notifications for Linux desktop tray items");
    channel.setSound(null, null);
    channel.enableVibration(false);
    manager.createNotificationChannel(channel);
  }

  private static Notification build(Context context, TrayItem item, String key) {
    String label = appLabel(context, item);
    TrayNotificationContent content = TrayNotificationContent.from(
        item.title, label, item.toolTip, item.status);
    int size = Math.max(1, Math.round(24 * context.getResources().getDisplayMetrics().density));
    Bitmap bitmap = item.statusIcon(context, size);
    Icon smallIcon = bitmap == null
        ? Icon.createWithResource(context, android.R.drawable.stat_notify_more)
        : Icon.createWithBitmap(bitmap);
    Intent activate = actionIntent(context, TrayNotificationReceiver.ACTION_ACTIVATE, key);
    PendingIntent contentIntent = PendingIntent.getBroadcast(context, REQUEST_ACTIVATE,
        activate, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
    Notification.Builder builder = new Notification.Builder(context, CHANNEL_ID)
        .setSmallIcon(smallIcon)
        .setContentTitle(content.title)
        .setContentIntent(contentIntent)
        .setOngoing(true)
        .setOnlyAlertOnce(true)
        .setDefaults(0)
        .setShowWhen(false)
        .setCategory(Notification.CATEGORY_SERVICE);
    if (content.text != null) builder.setContentText(content.text);
    if (content.subText != null) builder.setSubText(content.subText);
    if (hasMenu(item)) {
      PendingIntent menuIntent = PendingIntent.getBroadcast(context, REQUEST_MENU,
          actionIntent(context, TrayNotificationReceiver.ACTION_MENU, key),
          PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
      builder.addAction(new Notification.Action.Builder(
          Icon.createWithResource(context, android.R.drawable.ic_menu_more), "Menu", menuIntent).build());
    }
    return builder.build();
  }

  private static Intent actionIntent(Context context, String action, String key) {
    Uri data = new Uri.Builder().scheme("maton-tray").authority("notification")
        .appendPath(action.substring(action.lastIndexOf('.') + 1)).appendPath(key).build();
    return new Intent(context, TrayNotificationReceiver.class)
        .setAction(action).setData(data).putExtra(TrayMenuActivity.EXTRA_ITEM_KEY, key);
  }

  private static boolean hasMenu(TrayItem item) {
    return (item.menuPath != null && !item.menuPath.isEmpty() && !"/".equals(item.menuPath))
        || (item.menu != null && !item.menu.isEmpty());
  }

  private static String appLabel(Context context, TrayItem item) {
    if (item.bus == null) return "";
    String[] packages = context.getPackageManager().getPackagesForUid(item.bus.uid);
    if (packages == null) return "";
    java.util.Arrays.sort(packages);
    for (String packageName : packages) {
      try {
        CharSequence label = context.getPackageManager().getApplicationLabel(
            context.getPackageManager().getApplicationInfo(packageName, 0));
        if (label != null && !label.toString().trim().isEmpty()) return label.toString();
      } catch (PackageManager.NameNotFoundException error) {
        Log.e(TrayService.TAG, "Tray app label lookup failed package=" + packageName
            + " uid=" + item.bus.uid, error);
      } catch (RuntimeException error) {
        Log.e(TrayService.TAG, "Tray app label read failed package=" + packageName
            + " uid=" + item.bus.uid, error);
      }
    }
    return "";
  }
}
