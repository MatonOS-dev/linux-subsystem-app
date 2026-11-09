package org.matonos.tray;

import android.graphics.Bitmap;
import android.os.Bundle;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/** Bundle representation of the existing StatusNotifierItem model for Launcher3. */
final class TrayHostItems {
  private TrayHostItems() {}

  static List<Bundle> snapshot() {
    List<TrayItem> ordered = new ArrayList<>(TrayService.ITEMS.values());
    ordered.sort(Comparator.comparingLong(item -> item.registrationOrder));
    List<Bundle> result = new ArrayList<>(ordered.size());
    for (TrayItem item : ordered) {
      Bundle bundle = toBundle(item);
      if (bundle != null) {
        result.add(bundle);
      }
    }
    return result;
  }

  static Bundle toBundle(TrayItem item) {
    Bundle bundle = new Bundle();
    BundleWriter writer =
        new BundleWriter() {
          @Override
          public void putString(String key, String value) {
            bundle.putString(key, value);
          }

          @Override
          public void putParcelable(String key, android.os.Parcelable value) {
            bundle.putParcelable(key, value);
          }
        };
    return writeToBundle(item, writer) ? bundle : null;
  }

  static boolean writeToBundle(TrayItem item, BundleWriter bundle) {
    if ("Passive".equalsIgnoreCase(item.status)) {
      return false;
    }
    bundle.putString("key", TrayService.key(item.attachId, item.service));
    bundle.putString("title", item.title);
    bundle.putString("status", item.status);
    Bitmap icon = item.icon;
    if (icon != null) {
      bundle.putParcelable("icon", tintBlack(icon));
    }
    if (item.iconName != null && !item.iconName.isEmpty()) {
      bundle.putString("iconName", item.iconName);
    }
    return true;
  }

  // TEMP: black icons so white tray icons show on the light taskbar (until the dark pill).
  private static Bitmap tintBlack(Bitmap source) {
    Bitmap out = Bitmap.createBitmap(source.getWidth(), source.getHeight(), Bitmap.Config.ARGB_8888);
    android.graphics.Paint paint = new android.graphics.Paint(android.graphics.Paint.FILTER_BITMAP_FLAG);
    paint.setColorFilter(new android.graphics.PorterDuffColorFilter(
        android.graphics.Color.BLACK, android.graphics.PorterDuff.Mode.SRC_IN));
    new android.graphics.Canvas(out).drawBitmap(source, 0, 0, paint);
    return out;
  }

  interface BundleWriter {
    void putString(String key, String value);

    void putParcelable(String key, android.os.Parcelable value);
  }
}
