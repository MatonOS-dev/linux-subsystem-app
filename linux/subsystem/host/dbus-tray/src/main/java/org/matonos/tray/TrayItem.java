package org.matonos.tray;

import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.util.Log;
import java.util.ArrayList;
import java.util.List;

/** Mutable snapshot of one StatusNotifierItem on one attached private bus. */
public final class TrayItem {
  public final long attachId;
  public final String service;
  public final String path;
  public final String owner;
  public volatile String id = "";
  public volatile String title = "";
  public volatile String status = "Active";
  public volatile String iconName = "";
  public volatile String toolTip = "";
  public volatile String menuPath = "";
  public volatile boolean itemIsMenu;
  public volatile Bitmap icon;
  public volatile List<MenuEntry> menu = new ArrayList<>();
  TrayService.Bus bus;

  TrayItem(long attachId, String service, String path, String owner, TrayService.Bus bus) {
    this.attachId = attachId;
    this.service = service;
    this.path = path;
    this.owner = owner;
    this.bus = bus;
  }

  public void activate(int x, int y) {
    invoke("Activate", x, y);
  }

  public void secondaryActivate(int x, int y) {
    invoke("SecondaryActivate", x, y);
  }

  public void contextMenu(int x, int y) {
    invoke("ContextMenu", x, y);
  }

  void activateFromWidget(Context context) {
    Intent menuIntent =
        new Intent(context, TrayMenuActivity.class)
            .putExtra(TrayMenuActivity.EXTRA_ITEM_KEY, TrayService.key(attachId, service));
    menuIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
    context.startActivity(menuIntent);
  }

  public void scroll(int delta, String orientation) {
    new Thread(
            () -> {
              try {
                if (bus != null) {
                  bus.invokeScroll(this, delta, orientation);
                }
              } catch (Exception e) {
                Log.e(TrayService.TAG, "Scroll failed service=" + service + " path=" + path, e);
              }
            },
            "tray-scroll")
        .start();
  }

  private void invoke(String method, int x, int y) {
    new Thread(
            () -> {
              try {
                if (bus != null) {
                  bus.invokeItem(this, method, x, y);
                }
              } catch (Exception e) {
                Log.e(TrayService.TAG, method + " failed service=" + service + " path=" + path, e);
              }
            },
            "tray-action")
        .start();
  }

  public static final class MenuEntry {
    public final int id;
    public final String label;
    public final boolean enabled;
    public final boolean visible;
    public final boolean separator;
    public final String toggleType;
    public final int toggleState;
    public final Bitmap icon;
    public final List<MenuEntry> children;

    MenuEntry(
        int id,
        String label,
        boolean enabled,
        boolean visible,
        boolean separator,
        String toggleType,
        int toggleState,
        Bitmap icon,
        List<MenuEntry> children) {
      this.id = id;
      this.label = label;
      this.enabled = enabled;
      this.visible = visible;
      this.separator = separator;
      this.toggleType = toggleType;
      this.toggleState = toggleState;
      this.icon = icon;
      this.children = children;
    }
  }
}
