package org.matonos.tray;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;

import org.junit.Test;

public final class TrayNotificationContentTest {
  @Test
  public void mapsTitleTooltipAndAttentionStatus() {
    TrayNotificationContent content = TrayNotificationContent.from(
        "Download", "Steam", "Download complete", "NeedsAttention");
    assertEquals("Download", content.title);
    assertEquals("Download complete", content.text);
    assertEquals("Needs attention", content.subText);
  }

  @Test
  public void usesAppLabelWhenTitleIsMissingAndOmitsEmptyTooltip() {
    TrayNotificationContent content = TrayNotificationContent.from("", "Steam", "  ", "Active");
    assertEquals("Steam", content.title);
    assertNull(content.text);
    assertNull(content.subText);
  }

  @Test
  public void hasStableGenericTitleWhenBothNamesAreMissing() {
    TrayNotificationContent content = TrayNotificationContent.from(null, null, null, "Active");
    assertEquals("Tray item", content.title);
  }
}
