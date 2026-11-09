package org.matonos.tray;

import android.content.ContentResolver;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import com.caverock.androidsvg.SVG;
import java.io.FileNotFoundException;
import java.io.InputStream;
import java.util.concurrent.ConcurrentHashMap;

/** Loads icons of the active (user-chosen) icon theme from the subsystem's signature-protected provider. */
final class IconThemeResolver {
    static final Uri ACTIVE_URI = Uri.parse("content://org.matonos.linux.icons/active");
    private static final String NONE = "";
    private static volatile String theme;
    private static final String TAG = "MatonTrayIcons";
    private static final ConcurrentHashMap<String, Result> CACHE = new ConcurrentHashMap<>();
    private static final Result MISSING = new Result(null, false);

    private IconThemeResolver() {}

    static Result resolve(Context context, String name, int sizePx) {
        if (name == null || name.isEmpty() || sizePx <= 0) return MISSING;
        String active = activeTheme(context.getContentResolver());
        if (active.isEmpty()) return MISSING;
        String key = active + '\0' + name + '\0' + sizePx;
        Result cached = CACHE.get(key);
        if (cached != null) return cached;
        Result result = load(context.getContentResolver(), active, name + "-symbolic", sizePx, true);
        if (result.bitmap == null) result = load(context.getContentResolver(), active, name, sizePx, false);
        if (result.bitmap == null) result = MISSING;
        Result raced = CACHE.putIfAbsent(key, result);
        return raced == null ? result : raced;
    }

    static void clearCache() {
        theme = null;
        CACHE.clear();
    }

    /** The user's chosen theme id, or "" when none is chosen (themed icons off). */
    private static String activeTheme(ContentResolver resolver) {
        String cached = theme;
        if (cached != null) return cached;
        String value = NONE;
        try {
            android.os.Bundle out = resolver.call(ACTIVE_URI, "getActiveTheme", null, null);
            String chosen = out == null ? null : out.getString("theme");
            if (chosen != null) value = chosen;
        } catch (IllegalArgumentException missingProvider) {
            // Subsystem not installed: no icon themes (expected).
        } catch (Exception error) {
            Log.e(TAG, "Cannot read active icon theme from org.matonos.linux.icons", error);
        }
        theme = value;
        return value;
    }

    private static Result load(ContentResolver resolver, String themeId, String name, int size, boolean symbolic) {
        Uri uri = new Uri.Builder().scheme("content").authority("org.matonos.linux.icons")
                .appendPath("icon").appendPath(themeId).appendPath(name)
                .appendPath(Integer.toString(size)).appendPath("1").build();
        try (ParcelFileDescriptor descriptor = resolver.openFileDescriptor(uri, "r")) {
            if (descriptor == null) return MISSING;
            Bitmap bitmap;
            String mime = resolver.getType(uri);
            try (InputStream input = new android.os.ParcelFileDescriptor.AutoCloseInputStream(descriptor)) {
                if ("image/svg+xml".equals(mime)) {
                    SVG svg = SVG.getFromInputStream(input);
                    bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
                    svg.renderToCanvas(new Canvas(bitmap));
                } else {
                    bitmap = BitmapFactory.decodeStream(input);
                }
            }
            return new Result(bitmap, symbolic && bitmap != null);
        } catch (FileNotFoundException missing) {
            return MISSING;
        } catch (Exception error) {
            Log.e(TAG, "Cannot render themed icon theme=" + themeId + " name=" + name, error);
            return MISSING;
        }
    }

    static final class Result {
        final Bitmap bitmap;
        final boolean symbolic;
        Result(Bitmap bitmap, boolean symbolic) { this.bitmap = bitmap; this.symbolic = symbolic; }
    }
}
