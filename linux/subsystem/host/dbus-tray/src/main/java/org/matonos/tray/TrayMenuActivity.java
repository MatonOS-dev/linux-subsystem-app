package org.matonos.tray;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Typeface;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.Window;
import android.view.WindowManager;
import android.widget.BaseAdapter;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;

/** Dialog-themed recursive DBusMenu popup opened by a home-screen widget tap. */
public final class TrayMenuActivity extends Activity {
  static final String EXTRA_ITEM_KEY = "item_key";
  /** Open the item's menu without Activate, as plasma's right-click does. */
  static final String EXTRA_OPEN_MENU = "open_menu";
  private TrayItem item;
  private AlertDialog currentDialog;
  private int currentMenuParentId;
  private final BroadcastReceiver updates =
      new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
          if (item != null && currentDialog != null && currentDialog.isShowing()) {
            showMenu(
                entriesFor(currentMenuParentId),
                titleFor(currentMenuParentId),
                currentMenuParentId);
          }
        }
      };

  @Override
  protected void onCreate(Bundle state) {
    super.onCreate(state);
    getWindow().requestFeature(Window.FEATURE_NO_TITLE);
    item = TrayService.ITEMS.get(getIntent().getStringExtra(EXTRA_ITEM_KEY));
    if (item == null) {
      Log.e(
          TrayService.TAG,
          "menu Activity item missing package="
              + getPackageName()
              + " key="
              + getIntent().getStringExtra(EXTRA_ITEM_KEY));
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
    if (currentDialog != null) {
      currentDialog.setOnDismissListener(null);
      currentDialog.dismiss();
      currentDialog = null;
    }
    item = TrayService.ITEMS.get(intent.getStringExtra(EXTRA_ITEM_KEY));
    if (item == null) {
      Log.e(
          TrayService.TAG,
          "menu Activity item missing package="
              + getPackageName()
              + " key="
              + intent.getStringExtra(EXTRA_ITEM_KEY));
      finish();
      return;
    }
    if (item.itemIsMenu || intent.getBooleanExtra(EXTRA_OPEN_MENU, false)) {
      openRootMenu();
    } else {
      activateOrOpenMenu();
    }
  }

  @Override
  protected void onDestroy() {
    try {
      unregisterReceiver(updates);
    } catch (IllegalArgumentException ignored) {
      // Activity may finish before registration if the item disappeared.
    }
    super.onDestroy();
  }

  private void activateOrOpenMenu() {
    if (item.bus == null) {
      Log.e(
          TrayService.TAG,
          "Activate failed package="
              + getPackageName()
              + " service="
              + item.service
              + ": attached bus is unavailable");
      finish();
      return;
    }
    new Thread(
            () -> {
              try {
                item.bus.invokeItem(item, "Activate", 0, 0);
                runOnUiThread(this::finish);
              } catch (Exception e) {
                if (isUnknownMethod(e)) {
                  runOnUiThread(this::openRootMenu);
                } else {
                  Log.e(
                      TrayService.TAG,
                      "Activate failed package="
                          + getPackageName()
                          + " service="
                          + item.service
                          + " path="
                          + item.path,
                      e);
                  runOnUiThread(this::finish);
                }
              }
            },
            "tray-widget-activate-" + item.attachId)
        .start();
  }

  private boolean isUnknownMethod(Throwable error) {
    for (Throwable current = error; current != null; current = current.getCause()) {
      String message = current.getMessage();
      if (message != null && message.contains("org.freedesktop.DBus.Error.UnknownMethod")) {
        return true;
      }
    }
    return false;
  }

  /** Popup window anchored at the tapped icon (launcher-supplied source bounds). */
  private void placePopup() {
    Window window = getWindow();
    window.setBackgroundDrawableResource(android.R.color.transparent);
    window.setLayout(
        WindowManager.LayoutParams.WRAP_CONTENT, WindowManager.LayoutParams.WRAP_CONTENT);
    android.graphics.Rect anchor = getIntent().getSourceBounds();
    if (anchor == null) {
      window.setGravity(Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
      return;
    }
    window.setGravity(Gravity.TOP | Gravity.START);
    WindowManager.LayoutParams params = window.getAttributes();
    params.x = anchor.left;
    params.y = anchor.bottom;
    window.setAttributes(params);
  }

  private void openRootMenu() {
    placePopup();
    if (item.bus == null || item.menuPath.isEmpty()) {
      showMenu(item.menu, item.title, 0);
      return;
    }
    new Thread(
            () -> {
              try {
                item.bus.menuAbout(item, 0);
              } catch (Exception e) {
                Log.e(
                    TrayService.TAG,
                    "AboutToShow failed package="
                        + getPackageName()
                        + " service="
                        + item.service
                        + " path="
                        + item.menuPath,
                    e);
              }
              runOnUiThread(() -> showMenu(item.menu, item.title, 0));
            },
            "tray-menu-open-" + item.attachId)
        .start();
  }

  private void showMenu(List<TrayItem.MenuEntry> entries, String title, int parentId) {
    if (isFinishing()) {
      return;
    }
    if (currentDialog != null && currentDialog.isShowing()) {
      currentDialog.setOnDismissListener(null);
      currentDialog.dismiss();
    }
    currentMenuParentId = parentId;
    List<MenuViewModel.Node> nodes = MenuViewModel.from(entries).children;
    List<MenuViewModel.Node> visible = new ArrayList<>();
    List<CharSequence> labels = new ArrayList<>();
    for (MenuViewModel.Node node : nodes) {
      visible.add(node);
      labels.add(labelFor(node));
    }
    BaseAdapter adapter =
        new BaseAdapter() {
          @Override
          public int getCount() {
            return visible.size();
          }

          @Override
          public Object getItem(int position) {
            return visible.get(position);
          }

          @Override
          public long getItemId(int position) {
            return visible.get(position).id;
          }

          @Override
          public android.view.View getView(
              int position, android.view.View convertView, android.view.ViewGroup parent) {
            MenuViewModel.Node node = visible.get(position);
            LinearLayout row = new LinearLayout(TrayMenuActivity.this);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(16, 8, 16, 8);
            if (node.icon != null) {
              ImageView icon = new ImageView(TrayMenuActivity.this);
              icon.setImageBitmap(node.icon);
              int size = (int) (24 * getResources().getDisplayMetrics().density);
              row.addView(icon, new LinearLayout.LayoutParams(size, size));
            }
            TextView text = new TextView(TrayMenuActivity.this);
            text.setText(labels.get(position));
            text.setEnabled(node.enabled && !node.separator);
            text.setTypeface(null, node.toggleState == 1 ? Typeface.BOLD : Typeface.NORMAL);
            if (node.separator) {
              text.setTextColor(0xff777777);
            }
            text.setPadding(12, 4, 4, 4);
            row.addView(
                text, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
            row.setEnabled(node.enabled && !node.separator);
            return row;
          }
        };
    AlertDialog dialog =
        new AlertDialog.Builder(this)
            .setTitle(title)
            .setAdapter(adapter, (ignored, which) -> select(visible.get(which)))
            .create();
    dialog.setCanceledOnTouchOutside(true);
    dialog.setOnDismissListener(
        ignored -> {
          if (currentDialog == dialog) {
            currentDialog = null;
            finish();
          }
        });
    currentDialog = dialog;
    dialog.show();
  }

  private CharSequence labelFor(MenuViewModel.Node node) {
    if (node.separator) {
      return "────────────────";
    }
    String label = node.label;
    if ("checkmark".equals(node.toggleType)) {
      label = (node.toggleState == 1 ? "☑ " : "☐ ") + label;
    } else if ("radio".equals(node.toggleType)) {
      label = (node.toggleState == 1 ? "◉ " : "○ ") + label;
    }
    if (!node.children.isEmpty()) {
      label += "    ›";
    }
    return label;
  }

  private List<TrayItem.MenuEntry> entriesFor(int parentId) {
    return parentId == 0 ? item.menu : findChildren(item.menu, parentId);
  }

  private String titleFor(int parentId) {
    if (parentId == 0) {
      return item.title;
    }
    TrayItem.MenuEntry parent = findEntry(item.menu, parentId);
    return parent == null ? item.title : parent.label;
  }

  private TrayItem.MenuEntry findEntry(List<TrayItem.MenuEntry> entries, int id) {
    for (TrayItem.MenuEntry entry : entries) {
      if (entry.id == id) {
        return entry;
      }
      TrayItem.MenuEntry nested = findEntry(entry.children, id);
      if (nested != null) {
        return nested;
      }
    }
    return null;
  }

  private void select(MenuViewModel.Node node) {
    if (node.separator || !node.enabled) {
      return;
    }
    if (!node.children.isEmpty()) {
      int id = node.id;
      if (item.bus == null) {
        showMenu(findChildren(item.menu, id), node.label, id);
        return;
      }
      currentMenuParentId = id;
      new Thread(
              () -> {
                boolean updated = false;
                try {
                  updated = item.bus.menuAbout(item, id);
                } catch (Exception e) {
                  Log.e(
                      TrayService.TAG,
                      "submenu AboutToShow failed package="
                          + getPackageName()
                          + " service="
                          + item.service
                          + " item="
                          + id,
                      e);
                }
                if (!updated) {
                  runOnUiThread(() -> showMenu(findChildren(item.menu, id), node.label, id));
                }
              },
              "tray-menu-submenu-" + id)
          .start();
      return;
    }
    if (currentDialog != null) {
      currentDialog.dismiss();
    }
    new Thread(
            () -> {
              try {
                if (item.bus != null) {
                  item.bus.menuEvent(item, node.id);
                }
              } catch (Exception e) {
                Log.e(
                    TrayService.TAG,
                    "menu Event failed package="
                        + getPackageName()
                        + " service="
                        + item.service
                        + " id="
                        + node.id,
                    e);
              }
            },
            "tray-menu-click-" + node.id)
        .start();
  }

  private List<TrayItem.MenuEntry> findChildren(List<TrayItem.MenuEntry> entries, int id) {
    for (TrayItem.MenuEntry entry : entries) {
      if (entry.id == id) {
        return entry.children;
      }
      List<TrayItem.MenuEntry> nested = findChildren(entry.children, id);
      if (nested != null) {
        return nested;
      }
    }
    return new ArrayList<>();
  }
}
