package org.matonos.tray;

import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.util.Log;
import java.util.ArrayList;
import java.util.List;

/** Mutable snapshot of one StatusNotifierItem on one attached private bus. */
public final class TrayItem {
  private static final java.util.concurrent.atomic.AtomicLong NEXT_REGISTRATION_ORDER =
      new java.util.concurrent.atomic.AtomicLong();
  public final long attachId;
  public final long registrationOrder = NEXT_REGISTRATION_ORDER.getAndIncrement();
  public final String service;
  public final String path;
  public final String owner;
  public volatile String id = "";
  public volatile String title = "";
  public volatile String status = "Active";
  public volatile String iconName = "";
  public volatile String iconThemePath = "";
  public volatile String attentionIconName = "";
  public volatile String overlayIconName = "";
  public volatile String toolTip = "";
  public volatile String menuPath = "";
  public volatile boolean itemIsMenu;
  public volatile Bitmap icon;
  private final java.util.concurrent.ConcurrentHashMap<Integer, Bitmap> statusIcons =
      new java.util.concurrent.ConcurrentHashMap<>();
  final java.util.concurrent.atomic.AtomicBoolean loggedInitialProperties =
      new java.util.concurrent.atomic.AtomicBoolean();
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

  Bitmap statusIcon(Context context, int sizePx) {
    Bitmap cached = statusIcons.get(sizePx);
    if (cached != null && !cached.isRecycled()) return cached;
    Bitmap source = IconThemeResolver.resolve(context, iconName, sizePx).bitmap;
    if (source == null) source = icon;
    if (source == null && bus != null) {
      source = AppIconResolver.resolve(context, bus.uid, sizePx).bitmap;
    }
    Bitmap resolved = whiteAlphaMask(source, sizePx);
    if (resolved != null) {
      Bitmap raced = statusIcons.putIfAbsent(sizePx, resolved);
      return raced == null ? resolved : raced;
    }
    return null;
  }

  void invalidateStatusIcons() {
    statusIcons.clear();
  }

  private static Bitmap whiteAlphaMask(Bitmap source, int sizePx) {
    if (source == null || source.isRecycled() || sizePx <= 0) return null;
    Bitmap mask = Bitmap.createBitmap(sizePx, sizePx, Bitmap.Config.ARGB_8888);
    Canvas canvas = new Canvas(mask);
    android.graphics.Paint paint = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG
        | android.graphics.Paint.FILTER_BITMAP_FLAG);
    paint.setColorFilter(new android.graphics.PorterDuffColorFilter(Color.WHITE,
        android.graphics.PorterDuff.Mode.SRC_IN));
    canvas.drawBitmap(source, null, new android.graphics.Rect(0, 0, sizePx, sizePx), paint);
    return mask;
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
    public final String iconName;
    public final String shortcut;
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
      this(id, label, enabled, visible, separator, toggleType, toggleState, icon, "", "", children);
    }

    MenuEntry(
        int id,
        String label,
        boolean enabled,
        boolean visible,
        boolean separator,
        String toggleType,
        int toggleState,
        Bitmap icon,
        String iconName,
        List<MenuEntry> children) {
      this(id, label, enabled, visible, separator, toggleType, toggleState, icon, iconName, "", children);
    }

    MenuEntry(
        int id,
        String label,
        boolean enabled,
        boolean visible,
        boolean separator,
        String toggleType,
        int toggleState,
        Bitmap icon,
        String iconName,
        String shortcut,
        List<MenuEntry> children) {
      this.id = id;
      this.label = label;
      this.enabled = enabled;
      this.visible = visible;
      this.separator = separator;
      this.toggleType = toggleType;
      this.toggleState = toggleState;
      this.icon = icon;
      this.iconName = iconName;
      this.shortcut = shortcut;
      this.children = children;
    }
  }
}
