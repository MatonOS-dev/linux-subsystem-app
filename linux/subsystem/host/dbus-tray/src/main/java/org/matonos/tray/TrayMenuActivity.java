package org.matonos.tray;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.res.Resources;
import android.graphics.Color;
import android.graphics.Rect;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.util.DisplayMetrics;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;

/** Compact desktop-style DBusMenu popup opened at the tray item's screen bounds. */
public final class TrayMenuActivity extends Activity {
  static final String EXTRA_ITEM_KEY = "item_key";
  static final String EXTRA_OPEN_MENU = "open_menu";
  private static final int FALLBACK_SURFACE = 0xff252525;
  private static final int FALLBACK_TEXT = 0xfff5f5f5;
  private static final int FALLBACK_MUTED = 0xffa5a5a5;
  private static final int FALLBACK_BORDER = 0xff484848;
  private TrayItem item;
  private int currentMenuParentId;
  private boolean hadWindowFocus;
  private final List<Integer> parentStack = new ArrayList<>();
  private final BroadcastReceiver updates = new BroadcastReceiver() {
    @Override
    public void onReceive(Context context, Intent intent) {
      if (item != null) showMenu(entriesFor(currentMenuParentId), currentMenuParentId);
    }
  };

  @Override
  protected void onCreate(Bundle state) {
    super.onCreate(state);
    Window window = getWindow();
    window.requestFeature(Window.FEATURE_NO_TITLE);
    window.setBackgroundDrawableResource(android.R.color.transparent);
    window.setDimAmount(0f);
    window.addFlags(WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
        | WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH);
    item = TrayService.ITEMS.get(getIntent().getStringExtra(EXTRA_ITEM_KEY));
    if (item == null) {
      Log.e(TrayService.TAG, "menu Activity item missing package=" + getPackageName()
          + " key=" + getIntent().getStringExtra(EXTRA_ITEM_KEY));
      finish();
      return;
    }
    registerReceiver(updates, new IntentFilter(TrayService.ACTION_ITEMS), RECEIVER_NOT_EXPORTED);
    if (!item.itemIsMenu && !getIntent().getBooleanExtra(EXTRA_OPEN_MENU, false)) {
      activateOrOpenMenu();
      return;
    }
    openRootMenu();
  }

  @Override
  protected void onNewIntent(Intent intent) {
    super.onNewIntent(intent);
    setIntent(intent);
    item = TrayService.ITEMS.get(intent.getStringExtra(EXTRA_ITEM_KEY));
    if (item == null) {
      Log.e(TrayService.TAG, "menu Activity item missing package=" + getPackageName()
          + " key=" + intent.getStringExtra(EXTRA_ITEM_KEY));
      finish();
      return;
    }
    parentStack.clear();
    if (item.itemIsMenu || intent.getBooleanExtra(EXTRA_OPEN_MENU, false)) openRootMenu();
    else activateOrOpenMenu();
  }

  @Override
  protected void onDestroy() {
    try {
      unregisterReceiver(updates);
    } catch (IllegalArgumentException ignored) {
      // The item can disappear before receiver registration completes.
    }
    super.onDestroy();
  }

  @Override
  public void onWindowFocusChanged(boolean hasFocus) {
    super.onWindowFocusChanged(hasFocus);
    if (hasFocus) hadWindowFocus = true;
    else if (hadWindowFocus && !isFinishing()) finish();
  }

  @Override
  public boolean dispatchTouchEvent(MotionEvent event) {
    if (event.getActionMasked() == MotionEvent.ACTION_OUTSIDE) {
      finish();
      return true;
    }
    return super.dispatchTouchEvent(event);
  }

  @Override
  public boolean dispatchKeyEvent(KeyEvent event) {
    if (event.getKeyCode() == KeyEvent.KEYCODE_ESCAPE && event.getAction() == KeyEvent.ACTION_UP) {
      finish();
      return true;
    }
    return super.dispatchKeyEvent(event);
  }

  private void activateOrOpenMenu() {
    if (item.bus == null) {
      Log.e(TrayService.TAG, "Activate failed package=" + getPackageName() + " service="
          + item.service + ": attached bus is unavailable");
      finish();
      return;
    }
    new Thread(() -> {
      try {
        item.bus.invokeItem(item, "Activate", 0, 0);
        runOnUiThread(this::finish);
      } catch (Exception e) {
        if (isUnknownMethod(e)) runOnUiThread(this::openRootMenu);
        else {
          Log.e(TrayService.TAG, "Activate failed package=" + getPackageName() + " service="
              + item.service + " path=" + item.path, e);
          runOnUiThread(this::finish);
        }
      }
    }, "tray-widget-activate-" + item.attachId).start();
  }

  private boolean isUnknownMethod(Throwable error) {
    for (Throwable current = error; current != null; current = current.getCause()) {
      String message = current.getMessage();
      if (message != null && message.contains("org.freedesktop.DBus.Error.UnknownMethod")) return true;
    }
    return false;
  }

  private void openRootMenu() {
    parentStack.clear();
    currentMenuParentId = 0;
    if (item.bus == null || item.menuPath.isEmpty()) {
      showMenu(item.menu, 0);
      return;
    }
    new Thread(() -> {
      try {
        item.bus.menuAbout(item, 0);
      } catch (Exception e) {
        Log.e(TrayService.TAG, "AboutToShow failed package=" + getPackageName() + " service="
            + item.service + " path=" + item.menuPath, e);
      }
      runOnUiThread(() -> showMenu(item.menu, 0));
    }, "tray-menu-open-" + item.attachId).start();
  }

  private void showMenu(List<TrayItem.MenuEntry> entries, int parentId) {
    if (isFinishing()) return;
    currentMenuParentId = parentId;
    int surface = themeColor("colorSurface", FALLBACK_SURFACE);
    int textColor = themeColor("colorOnSurface", FALLBACK_TEXT);
    int mutedColor = themeColor("colorOnSurfaceVariant", FALLBACK_MUTED);
    int borderColor = themeColor("colorOutline", FALLBACK_BORDER);
    int screenWidth = screenWidth();
    int screenHeight = screenHeight();
    int maxWidth = Math.min(dp(360), screenWidth);
    int maxHeight = Math.max(dp(120), screenHeight - dp(24));

    LinearLayout card = new LinearLayout(this);
    card.setOrientation(LinearLayout.VERTICAL);
    card.setPadding(dp(4), dp(5), dp(4), dp(5));
    GradientDrawable background = new GradientDrawable();
    background.setColor(surface);
    background.setCornerRadius(dp(8));
    background.setStroke(dp(1), borderColor);
    card.setBackground(background);
    card.setElevation(dp(8));

    TextView header = new TextView(this);
    header.setText(item.title == null || item.title.isEmpty() ? item.id : item.title);
    header.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
    header.setTextColor(mutedColor);
    header.setGravity(Gravity.CENTER_VERTICAL);
    header.setPadding(dp(12), 0, dp(12), 0);
    card.addView(header, new LinearLayout.LayoutParams(-1, dp(34)));
    if (!parentStack.isEmpty()) {
      TextView back = new TextView(this);
      back.setText("‹  Back");
      back.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
      back.setTextColor(textColor);
      back.setGravity(Gravity.CENTER_VERTICAL);
      back.setPadding(dp(12), 0, dp(12), 0);
      back.setOnClickListener(view -> {
        int prior = parentStack.remove(parentStack.size() - 1);
        showMenu(entriesFor(prior), prior);
      });
      card.addView(back, new LinearLayout.LayoutParams(-1, dp(36)));
    }

    LinearLayout rows = new LinearLayout(this);
    rows.setOrientation(LinearLayout.VERTICAL);
    for (MenuViewModel.Node node : MenuViewModel.from(entries).children) {
      if (node.separator) {
        View line = new View(this);
        line.setBackgroundColor(borderColor);
        LinearLayout.LayoutParams lineParams = new LinearLayout.LayoutParams(-1, dp(1));
        lineParams.setMargins(dp(10), dp(3), dp(10), dp(3));
        rows.addView(line, lineParams);
      } else {
        rows.addView(createRow(node, textColor, mutedColor));
      }
    }
    ScrollView scroll = new ScrollView(this);
    scroll.setFillViewport(false);
    scroll.addView(rows, new ScrollView.LayoutParams(-1, -2));
    card.addView(scroll, new LinearLayout.LayoutParams(-1, -2));

    card.measure(View.MeasureSpec.makeMeasureSpec(maxWidth, View.MeasureSpec.AT_MOST),
        View.MeasureSpec.makeMeasureSpec(maxHeight, View.MeasureSpec.AT_MOST));
    int width = Math.max(dp(220), Math.min(maxWidth, card.getMeasuredWidth()));
    int height = Math.min(maxHeight, Math.max(dp(42), card.getMeasuredHeight()));
    int widthSpec = View.MeasureSpec.makeMeasureSpec(width - dp(10), View.MeasureSpec.AT_MOST);
    card.measure(widthSpec, View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.AT_MOST));
    setContentView(card, new FrameLayout.LayoutParams(width, height));

    Window window = getWindow();
    window.setGravity(Gravity.TOP | Gravity.START);
    window.setLayout(width, height);
    Rect anchor = getIntent().getSourceBounds();
    if (anchor == null) {
      int left = (screenWidth - width) / 2;
      anchor = new Rect(left, screenHeight, left + width, screenHeight);
    }
    int[] placement = PopupPlacement.place(screenWidth, screenHeight,
        anchor.left, anchor.top, anchor.bottom, width, height);
    WindowManager.LayoutParams params = window.getAttributes();
    params.x = placement[0];
    params.y = placement[1];
    window.setAttributes(params);
  }

  private View createRow(MenuViewModel.Node node, int textColor, int mutedColor) {
    LinearLayout row = new LinearLayout(this);
    row.setGravity(Gravity.CENTER_VERTICAL);
    row.setPadding(dp(8), 0, dp(8), 0);
    row.setMinimumHeight(dp(36));
    row.setBackground(hoverBackground());
    row.setEnabled(node.enabled);
    row.setAlpha(node.enabled ? 1f : 0.45f);

    View indicator = null;
    if ("checkmark".equals(node.toggleType)) {
      Switch toggle = new Switch(this);
      toggle.setChecked(node.toggleState == 1);
      toggle.setClickable(false);
      indicator = toggle;
    } else if ("radio".equals(node.toggleType)) {
      TextView radio = new TextView(this);
      radio.setText(node.toggleState == 1 ? "●" : "○");
      radio.setTextColor(textColor);
      radio.setGravity(Gravity.CENTER);
      indicator = radio;
    } else if (node.icon != null || (node.iconName != null && !node.iconName.isEmpty())) {
      ImageView icon = new ImageView(this);
      if (node.icon != null && !node.icon.isRecycled()) icon.setImageBitmap(node.icon);
      else {
        IconThemeResolver.Result themed = IconThemeResolver.resolve(this, node.iconName, dp(18));
        if (themed.bitmap != null) icon.setImageBitmap(themed.bitmap);
      }
      indicator = icon;
    }
    LinearLayout.LayoutParams indicatorParams = new LinearLayout.LayoutParams(dp(24), dp(24));
    indicatorParams.setMargins(0, 0, dp(9), 0);
    if (indicator != null) row.addView(indicator, indicatorParams);
    else row.addView(new View(this), indicatorParams);

    TextView label = new TextView(this);
    label.setText(node.label);
    label.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
    label.setTextColor(textColor);
    label.setSingleLine(true);
    row.addView(label, new LinearLayout.LayoutParams(0, dp(36), 1));
    if (node.shortcut != null && !node.shortcut.isEmpty()) {
      TextView shortcut = new TextView(this);
      shortcut.setText(node.shortcut);
      shortcut.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
      shortcut.setTextColor(mutedColor);
      shortcut.setGravity(Gravity.CENTER_VERTICAL | Gravity.RIGHT);
      shortcut.setPadding(dp(8), 0, dp(4), 0);
      row.addView(shortcut, new LinearLayout.LayoutParams(-2, dp(36)));
    }
    if (!node.children.isEmpty()) {
      TextView arrow = new TextView(this);
      arrow.setText("›");
      arrow.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
      arrow.setTextColor(mutedColor);
      arrow.setGravity(Gravity.CENTER);
      row.addView(arrow, new LinearLayout.LayoutParams(dp(22), dp(36)));
    }
    row.setOnClickListener(node.enabled ? view -> select(node) : null);
    return row;
  }

  private Drawable hoverBackground() {
    GradientDrawable normal = new GradientDrawable();
    normal.setColor(Color.TRANSPARENT);
    normal.setCornerRadius(dp(5));
    GradientDrawable hovered = new GradientDrawable();
    hovered.setColor(themeColor("colorPrimary", 0xff414141));
    hovered.setCornerRadius(dp(5));
    android.graphics.drawable.StateListDrawable states = new android.graphics.drawable.StateListDrawable();
    states.addState(new int[] {android.R.attr.state_hovered}, hovered);
    states.addState(new int[] {}, normal);
    return states;
  }

  private int themeColor(String attribute, int fallback) {
    int id = getResources().getIdentifier(attribute, "attr", "android");
    if (id == 0) return fallback;
    TypedValue value = new TypedValue();
    return getTheme().resolveAttribute(id, value, true) ? value.data : fallback;
  }

  private int screenWidth() {
    DisplayMetrics metrics = new DisplayMetrics();
    getWindowManager().getDefaultDisplay().getRealMetrics(metrics);
    return metrics.widthPixels;
  }

  private int screenHeight() {
    DisplayMetrics metrics = new DisplayMetrics();
    getWindowManager().getDefaultDisplay().getRealMetrics(metrics);
    return metrics.heightPixels;
  }

  private void select(MenuViewModel.Node node) {
    if (node.separator || !node.enabled) return;
    if (!node.children.isEmpty()) {
      int id = node.id;
      if (item.bus == null) {
        parentStack.add(currentMenuParentId);
        showMenu(findChildren(item.menu, id), id);
        return;
      }
      int priorParentId = currentMenuParentId;
      parentStack.add(priorParentId);
      currentMenuParentId = id;
      new Thread(() -> {
        boolean updated = false;
        try {
          updated = item.bus.menuAbout(item, id);
        } catch (Exception e) {
          Log.e(TrayService.TAG, "submenu AboutToShow failed package=" + getPackageName()
              + " service=" + item.service + " item=" + id, e);
        }
        if (!updated) runOnUiThread(() -> showMenu(findChildren(item.menu, id), id));
      }, "tray-menu-submenu-" + id).start();
      return;
    }
    finish();
    new Thread(() -> {
      try {
        if (item.bus != null) item.bus.menuEvent(item, node.id);
      } catch (Exception e) {
        Log.e(TrayService.TAG, "menu Event failed package=" + getPackageName() + " service="
            + item.service + " id=" + node.id, e);
      }
    }, "tray-menu-click-" + node.id).start();
  }

  private List<TrayItem.MenuEntry> entriesFor(int parentId) {
    if (parentId == 0) return item.menu;
    List<TrayItem.MenuEntry> children = findChildren(item.menu, parentId);
    return children == null ? new ArrayList<>() : children;
  }

  private List<TrayItem.MenuEntry> findChildren(List<TrayItem.MenuEntry> entries, int id) {
    if (entries == null) return new ArrayList<>();
    for (TrayItem.MenuEntry entry : entries) {
      if (entry.id == id) return entry.children;
      List<TrayItem.MenuEntry> nested = findChildren(entry.children, id);
      if (nested != null) return nested;
    }
    return null;
  }

  private int dp(int value) {
    return Math.round(value * getResources().getDisplayMetrics().density);
  }
}
