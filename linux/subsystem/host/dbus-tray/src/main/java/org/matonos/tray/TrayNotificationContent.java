package org.matonos.tray;

/** Pure notification text mapping for a StatusNotifierItem snapshot. */
final class TrayNotificationContent {
  final String title;
  final String text;
  final String subText;

  private TrayNotificationContent(String title, String text, String subText) {
    this.title = title;
    this.text = text;
    this.subText = subText;
  }

  static TrayNotificationContent from(String itemTitle, String appLabel, String toolTip, String status) {
    String title = nonEmpty(itemTitle) ? itemTitle : appLabel;
    if (!nonEmpty(title)) title = "Tray item";
    String text = nonEmpty(toolTip) ? toolTip : null;
    String subText = "NeedsAttention".equalsIgnoreCase(status) ? "Needs attention" : null;
    return new TrayNotificationContent(title, text, subText);
  }

  private static boolean nonEmpty(String value) {
    return value != null && !value.trim().isEmpty();
  }
}
