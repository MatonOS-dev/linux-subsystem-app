package org.matonos.tray;

import android.content.Context;
import android.content.pm.PackageManager;
import android.content.pm.ApplicationInfo;
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
      String appId = flatpakId(context.getPackageManager(), packageName, uid);
      if (appId == null) continue;
      IconThemeResolver.Result themed = IconThemeResolver.resolveSymbolic(context, appId + "-symbolic", sizePx);
      if (themed.bitmap != null) {
        CACHE.putIfAbsent(uid, new Entry(packageName, themed.bitmap));
        return themed;
      }
      try {
        Drawable launcher = context.getPackageManager().getApplicationIcon(packageName);
        Bitmap bitmap;
        if (launcher instanceof AdaptiveIconDrawable) {
          Drawable foreground = ((AdaptiveIconDrawable) launcher).getForeground();
          if (foreground == null) {
            Log.e(TAG, "Adaptive launcher icon has no foreground package=" + packageName
                + " uid=" + uid);
            continue;
          }
          bitmap = silhouetteAdaptiveForeground(foreground, sizePx);
        } else {
          bitmap = silhouette(launcher, sizePx);
        }
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

  static String flatpakId(PackageManager packageManager, String packageName, int uid) {
    try {
      ApplicationInfo info = packageManager.getApplicationInfo(packageName, PackageManager.GET_META_DATA);
      String id = metadataFlatpakId(info.metaData == null ? null : info.metaData.getString("org.matonos.linux.flatpak_id"));
      if (id == null || id.isEmpty()) {
        Log.e(TAG, "Missing Flatpak id metadata package=" + packageName + " uid=" + uid);
        return null;
      }
      return id;
    } catch (PackageManager.NameNotFoundException error) {
      Log.e(TAG, "Flatpak metadata lookup failed package=" + packageName + " uid=" + uid, error);
      return null;
    }
  }

  static String metadataFlatpakId(String value) {
    return value == null || value.isEmpty() ? null : value;
  }

  private static Bitmap silhouette(Drawable source, int size) {
    Bitmap original = render(source, size);
    if (original == null) return null;
    return alphaSilhouette(original);
  }

  private static Bitmap silhouetteAdaptiveForeground(Drawable foreground, int size) {
    if (size <= 0) return null;
    int canvasSize = adaptiveForegroundCanvasSize(size);
    Bitmap full = render(foreground, canvasSize);
    if (full == null) return null;
    int offset = adaptiveForegroundCropOffset(size);
    Bitmap safeZone = Bitmap.createBitmap(full, offset, offset, size, size);
    full.recycle();
    return alphaSilhouette(safeZone);
  }

  private static Bitmap alphaSilhouette(Bitmap original) {
    int size = original.getWidth();
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

  static int adaptiveForegroundCanvasSize(int outputSize) {
    if (outputSize <= 0) return outputSize;
    // Adaptive icon artwork uses a 108dp layer with a 66dp safe zone.
    return (int) (((long) outputSize * 18 + 10) / 11);
  }

  static int adaptiveForegroundCropOffset(int outputSize) {
    return (adaptiveForegroundCanvasSize(outputSize) - outputSize) / 2;
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
