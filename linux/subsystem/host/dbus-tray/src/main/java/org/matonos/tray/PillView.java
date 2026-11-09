package org.matonos.tray;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffColorFilter;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/** Surface-hosted, compact StatusNotifierItem strip used by the desktop launcher. */
final class PillView extends View {
    private static final int TARGET_DP = 32;
    private static final int ICON_DP = 18;
    private static final long LONG_PRESS_MS = 500;
    private final TrayHostService host;
    private final IPillCallback callback;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final List<TrayItem> items = new ArrayList<>();
    private final Runnable longPress = this::handleLongPress;
    private int targetSize;
    private int iconSize;
    private int screenX;
    private int screenY;
    private int pressedIndex = -1;
    private float downX;
    private float downY;
    private boolean longPressTriggered;
    private int lastWidth = -1;

    private void handleLongPress() {
        if (pressedIndex >= 0 && pressedIndex < items.size()) {
            host.performPillAction(items.get(pressedIndex), itemBounds(pressedIndex), true);
            longPressTriggered = true;
        }
    }

    PillView(Context context, TrayHostService host, int heightPx, IPillCallback callback) {
        super(context);
        this.host = host;
        this.callback = callback;
        int densityTarget = dp(TARGET_DP);
        targetSize = heightPx > 0 ? Math.min(densityTarget, heightPx) : densityTarget;
        targetSize = Math.max(1, targetSize);
        iconSize = Math.min(dp(ICON_DP), targetSize);
        setFocusable(true);
        refreshItems();
    }

    void setScreenOrigin(int x, int y) {
        screenX = x;
        screenY = y;
    }

    int desiredWidth() {
        return targetSize * items.size();
    }

    int desiredHeight() {
        return targetSize;
    }

    void refreshItems() {
        items.clear();
        items.addAll(TrayService.ITEMS.values());
        items.removeIf(item -> "Passive".equalsIgnoreCase(item.status));
        items.sort(Comparator.comparingLong(item -> item.registrationOrder));
        int desiredWidth = targetSize * items.size();
        setMinimumWidth(desiredWidth);
        requestLayout();
        invalidate();
        if (desiredWidth != lastWidth) {
            lastWidth = desiredWidth;
            host.updatePillLayout(desiredWidth, targetSize);
            try {
                callback.onWidthChanged(desiredWidth);
            } catch (Exception e) {
                Log.e(TrayService.TAG, "Pill width callback failed package=org.matonos.tray width=" + desiredWidth, e);
            }
        }
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int width = targetSize * items.size();
        int height = targetSize;
        setMeasuredDimension(resolveSize(width, widthMeasureSpec), resolveSize(height, heightMeasureSpec));
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        float radius = targetSize / 2f;
        paint.setColor(0xff252525);
        paint.setColorFilter(null);
        canvas.drawRoundRect(new RectF(0, 0, getWidth(), getHeight()), radius, radius, paint);
        for (int i = 0; i < items.size(); i++) {
            TrayItem item = items.get(i);
            float centerX = i * targetSize + targetSize / 2f;
            Bitmap icon = resolveIcon(item);
            if (icon != null && !icon.isRecycled()) {
                Rect source = new Rect(0, 0, icon.getWidth(), icon.getHeight());
                Rect destination = new Rect(
                        Math.round(centerX - iconSize / 2f),
                        Math.round(getHeight() / 2f - iconSize / 2f),
                        Math.round(centerX + iconSize / 2f),
                        Math.round(getHeight() / 2f + iconSize / 2f));
                paint.setColorFilter(new PorterDuffColorFilter(Color.WHITE, PorterDuff.Mode.SRC_IN));
                canvas.drawBitmap(icon, source, destination, paint);
                paint.setColorFilter(null);
            } else {
                String label = item.iconName == null || item.iconName.isEmpty() ? item.title : item.iconName;
                if (label == null || label.isEmpty()) label = "•";
                paint.setColor(Color.WHITE);
                paint.setTextSize(iconSize);
                paint.setTextAlign(Paint.Align.CENTER);
                canvas.drawText(label.substring(0, 1), centerX, getHeight() / 2f - (paint.ascent() + paint.descent()) / 2f, paint);
            }
            if ("NeedsAttention".equalsIgnoreCase(item.status)) {
                paint.setColor(0xffffb74d);
                canvas.drawCircle(i * targetSize + targetSize - dp(6), dp(6), dp(3), paint);
            }
        }
    }

    private Bitmap resolveIcon(TrayItem item) {
        IconThemeResolver.Result themed = IconThemeResolver.resolve(getContext(), item.iconName, iconSize);
        return themed.bitmap != null ? themed.bitmap : item.icon;
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int index = (int) (event.getX() / Math.max(1, targetSize));
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                pressedIndex = validIndex(index) ? index : -1;
                downX = event.getX();
                downY = event.getY();
                longPressTriggered = false;
                if (pressedIndex >= 0 && event.getButtonState() == MotionEvent.BUTTON_SECONDARY) {
                    host.performPillAction(items.get(pressedIndex), itemBounds(pressedIndex), true);
                    longPressTriggered = true;
                    return true;
                }
                if (pressedIndex >= 0) handler.postDelayed(longPress, LONG_PRESS_MS);
                return true;
            case MotionEvent.ACTION_MOVE:
                if (Math.abs(event.getX() - downX) > dp(8) || Math.abs(event.getY() - downY) > dp(8)) {
                    handler.removeCallbacks(longPress);
                }
                return true;
            case MotionEvent.ACTION_UP:
                handler.removeCallbacks(longPress);
                if (!longPressTriggered && validIndex(pressedIndex)) {
                    if (Math.abs(event.getX() - downX) > dp(8) || Math.abs(event.getY() - downY) > dp(8)) {
                        float dx = event.getX() - downX;
                        float dy = event.getY() - downY;
                        boolean horizontal = Math.abs(dx) >= Math.abs(dy);
                        int delta = Math.round((horizontal ? dx : dy) * 120 / targetSize);
                        items.get(pressedIndex).scroll(delta, horizontal ? "horizontal" : "vertical");
                    } else {
                        host.performPillAction(items.get(pressedIndex), itemBounds(pressedIndex), false);
                    }
                }
                pressedIndex = -1;
                performClick();
                return true;
            case MotionEvent.ACTION_CANCEL:
                handler.removeCallbacks(longPress);
                pressedIndex = -1;
                return true;
            default:
                return true;
        }
    }

    @Override
    public boolean performClick() {
        super.performClick();
        return true;
    }

    private Rect itemBounds(int index) {
        int left = screenX + index * targetSize;
        return new Rect(left, screenY, left + targetSize, screenY + targetSize);
    }

    private boolean validIndex(int index) {
        return index >= 0 && index < items.size();
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
