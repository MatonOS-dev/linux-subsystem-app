package org.matonos.tray;

import android.app.PendingIntent;
import android.appwidget.AppWidgetManager;
import android.appwidget.AppWidgetProvider;
import android.content.ComponentName;
import android.content.Context;
import android.graphics.Bitmap;
import android.net.Uri;
import android.widget.RemoteViews;
import java.util.ArrayList;
import java.util.List;

/** Home-screen strip containing active StatusNotifierItems from every attached bus. */
public final class TrayWidgetProvider extends AppWidgetProvider {
  @Override
  public void onUpdate(Context context, AppWidgetManager manager, int[] ids) {
    for (int id : ids) {
      manager.updateAppWidget(id, makeViews(context));
    }
  }

  @Override
  public void onReceive(Context context, android.content.Intent intent) {
    super.onReceive(context, intent);
    if (TrayService.ACTION_ITEMS.equals(intent.getAction())) {
      AppWidgetManager manager = AppWidgetManager.getInstance(context);
      int[] ids = manager.getAppWidgetIds(new ComponentName(context, TrayWidgetProvider.class));
      onUpdate(context, manager, ids);
    }
  }

  private RemoteViews makeViews(Context context) {
    RemoteViews views = new RemoteViews(context.getPackageName(), R.layout.tray_widget);
    views.removeAllViews(R.id.tray_items);
    List<TrayItem> items = new ArrayList<>(TrayService.ITEMS.values());
    items.sort(
        (left, right) -> (left.attachId + left.service).compareTo(right.attachId + right.service));
    for (TrayItem item : items) {
      if ("Passive".equalsIgnoreCase(item.status)) {
        continue;
      }
      RemoteViews child = new RemoteViews(context.getPackageName(), R.layout.tray_widget_item);
      Bitmap icon = item.icon;
      if (icon != null) {
        child.setImageViewBitmap(R.id.tray_icon, icon);
        child.setViewVisibility(R.id.tray_icon, android.view.View.VISIBLE);
        child.setViewVisibility(R.id.tray_icon_name, android.view.View.GONE);
      } else {
        child.setViewVisibility(R.id.tray_icon, android.view.View.GONE);
        child.setTextViewText(
            R.id.tray_icon_name, item.iconName.isEmpty() ? item.title : item.iconName);
        child.setViewVisibility(R.id.tray_icon_name, android.view.View.VISIBLE);
      }
      boolean needsAttention = "NeedsAttention".equalsIgnoreCase(item.status);
      child.setTextViewText(R.id.tray_attention, needsAttention ? "•" : "");
      String key = TrayService.key(item.attachId, item.service);
      android.content.Intent tap =
          new android.content.Intent(context, TrayMenuActivity.class)
              .setData(Uri.parse("matonos-tray://item/" + Uri.encode(key)))
              .putExtra(TrayMenuActivity.EXTRA_ITEM_KEY, key);
      PendingIntent pending =
          PendingIntent.getActivity(
              context,
              (int) (item.attachId * 31 + item.service.hashCode()),
              tap,
              PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
      child.setOnClickPendingIntent(R.id.tray_item, pending);
      views.addView(R.id.tray_items, child);
    }
    return views;
  }
}
