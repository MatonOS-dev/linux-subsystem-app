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

/** Loads Papirus icons from the subsystem's signature-protected provider. */
final class IconThemeResolver {
    static final String THEME = "Papirus";
    private static final String TAG = "MatonTrayIcons";
    private static final ConcurrentHashMap<String, Result> CACHE = new ConcurrentHashMap<>();
    private static final Result MISSING = new Result(null, false);

    private IconThemeResolver() {}

    static Result resolve(Context context, String name, int sizePx) {
        if (name == null || name.isEmpty() || sizePx <= 0) return MISSING;
        String key = THEME + '\0' + name + '\0' + sizePx;
        Result cached = CACHE.get(key);
        if (cached != null) return cached;
        Result result = load(context.getContentResolver(), name + "-symbolic", sizePx, true);
        if (result.bitmap == null) result = load(context.getContentResolver(), name, sizePx, false);
        if (result.bitmap == null) result = MISSING;
        Result raced = CACHE.putIfAbsent(key, result);
        return raced == null ? result : raced;
    }

    static void clearCache() { CACHE.clear(); }

    private static Result load(ContentResolver resolver, String name, int size, boolean symbolic) {
        Uri uri = new Uri.Builder().scheme("content").authority("org.matonos.linux.icons")
                .appendPath("icon").appendPath(THEME).appendPath(name)
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
            Log.e(TAG, "Cannot render themed icon theme=" + THEME + " name=" + name, error);
            return MISSING;
        }
    }

    static final class Result {
        final Bitmap bitmap;
        final boolean symbolic;
        Result(Bitmap bitmap, boolean symbolic) { this.bitmap = bitmap; this.symbolic = symbolic; }
    }
}
