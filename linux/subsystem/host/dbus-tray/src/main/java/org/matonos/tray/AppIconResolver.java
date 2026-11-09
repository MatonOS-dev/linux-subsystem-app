package org.matonos.tray;

import android.content.Context;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.drawable.AdaptiveIconDrawable;
import android.graphics.drawable.Drawable;
import android.util.Log;
import java.util.Arrays;
import java.util.concurrent.ConcurrentHashMap;

/** Generic launcher-icon fallback for StatusNotifierItems attached by Flatpak apps. */
final class AppIconResolver {
  private static final String TAG = "MatonTrayIcons";
  private static final ConcurrentHashMap<Integer, Entry> CACHE = new ConcurrentHashMap<>();

  private AppIconResolver() {}

  static IconThemeResolver.Result resolve(Context context, int uid, int sizePx) {
    Entry cached = CACHE.get(uid);
    if (cached != null) return new IconThemeResolver.Result(cached.bitmap, true);
    String[] packages = context.getPackageManager().getPackagesForUid(uid);
    if (packages == null || packages.length == 0) {
      Log.e(TAG, "No Android package for tray owner uid=" + uid);
      return new IconThemeResolver.Result(null, false);
    }
    Arrays.sort(packages);
    for (String packageName : packages) {
      if (!packageName.startsWith("flatpak.app.")) continue;
      String appId = flatpakId(packageName);
      IconThemeResolver.Result themed = IconThemeResolver.resolveSymbolic(context, appId + "-symbolic", sizePx);
      if (themed.bitmap != null) {
        CACHE.putIfAbsent(uid, new Entry(packageName, themed.bitmap));
        return themed;
      }
      try {
        Drawable launcher = context.getPackageManager().getApplicationIcon(packageName);
        Drawable monochrome = launcher instanceof AdaptiveIconDrawable
            ? ((AdaptiveIconDrawable) launcher).getMonochrome() : null;
        Bitmap bitmap = monochrome == null ? silhouette(launcher, sizePx) : render(monochrome, sizePx);
        if (bitmap != null) {
          CACHE.putIfAbsent(uid, new Entry(packageName, bitmap));
          return new IconThemeResolver.Result(bitmap, true);
        }
      } catch (PackageManager.NameNotFoundException error) {
        Log.e(TAG, "Launcher icon lookup failed package=" + packageName + " uid=" + uid, error);
      } catch (RuntimeException error) {
        Log.e(TAG, "Launcher icon rendering failed package=" + packageName + " uid=" + uid, error);
      }
    }
    return new IconThemeResolver.Result(null, false);
  }

  static void clearCache() { CACHE.clear(); }

  static int firstAvailable(boolean themed, boolean pixmap, boolean launcherIcon) {
    if (themed) return 0;
    if (pixmap) return 1;
    if (launcherIcon) return 2;
    return 3;
  }

  private static String flatpakId(String packageName) {
    String[] parts = packageName.split("\\.");
    if (parts.length < 5) return packageName;
    // DeployManifest encodes dots and other separators in ref segments as underscores.
    return parts[2].replace('_', '.');
  }

  private static Bitmap silhouette(Drawable source, int size) {
    Bitmap original = render(source, size);
    if (original == null) return null;
    Bitmap mask = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
    int[] pixels = new int[size * size];
    original.getPixels(pixels, 0, size, 0, 0, size, size);
    for (int i = 0; i < pixels.length; i++) {
      int alpha = Color.alpha(pixels[i]);
      pixels[i] = alpha == 0 ? Color.TRANSPARENT : Color.argb(alpha, 255, 255, 255);
    }
    mask.setPixels(pixels, 0, size, 0, 0, size, size);
    original.recycle();
    return mask;
  }

  private static Bitmap render(Drawable drawable, int size) {
    if (size <= 0) return null;
    Bitmap bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
    Canvas canvas = new Canvas(bitmap);
    drawable.setBounds(0, 0, size, size);
    drawable.draw(canvas);
    return bitmap;
  }

  private static final class Entry {
    final String packageName;
    final Bitmap bitmap;
    Entry(String packageName, Bitmap bitmap) { this.packageName = packageName; this.bitmap = bitmap; }
  }
}
