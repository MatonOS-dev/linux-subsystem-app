package org.matonos.tray;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** UI-ready DBusMenu tree. Hidden nodes are omitted recursively. */
public final class MenuViewModel {
  public final List<Node> children;

  private MenuViewModel(List<Node> children) {
    this.children = Collections.unmodifiableList(children);
  }

  public static MenuViewModel from(List<TrayItem.MenuEntry> entries) {
    return new MenuViewModel(convert(entries));
  }

  private static List<Node> convert(List<TrayItem.MenuEntry> entries) {
    List<Node> result = new ArrayList<>();
    if (entries == null) {
      return result;
    }
    for (TrayItem.MenuEntry entry : entries) {
      if (!entry.visible) {
        continue;
      }
      result.add(
          new Node(
              entry.id,
              entry.label,
              entry.enabled,
              entry.separator,
              entry.toggleType,
              entry.toggleState,
              entry.icon,
              convert(entry.children)));
    }
    return result;
  }

  public static final class Node {
    public final int id;
    public final String label;
    public final boolean enabled;
    public final boolean separator;
    public final String toggleType;
    public final int toggleState;
    public final android.graphics.Bitmap icon;
    public final List<Node> children;

    Node(
        int id,
        String label,
        boolean enabled,
        boolean separator,
        String toggleType,
        int toggleState,
        android.graphics.Bitmap icon,
        List<Node> children) {
      this.id = id;
      this.label = label;
      this.enabled = enabled;
      this.separator = separator;
      this.toggleType = toggleType;
      this.toggleState = toggleState;
      this.icon = icon;
      this.children = Collections.unmodifiableList(children);
    }
  }
}
