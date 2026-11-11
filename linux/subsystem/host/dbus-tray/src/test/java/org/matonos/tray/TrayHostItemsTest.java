package org.matonos.tray;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.os.Parcelable;
import java.util.HashMap;
import java.util.Map;
import org.junit.Test;

public final class TrayHostItemsTest {
  @Test
  public void convertsItemAndOmitsPassiveAndMissingIcon() {
    TrayItem active = new TrayItem(7, "org.example.Tray", "/StatusNotifierItem", ":1.0", null);
    active.title = "Example";
    active.status = "Active";
    active.iconName = "example-symbolic";

    RecordingBundle bundle = new RecordingBundle();
    TrayHostItems.writeToBundle(active, null, bundle);

    assertEquals("7\norg.example.Tray", bundle.values.get("key"));
    assertEquals("Example", bundle.values.get("title"));
    assertEquals("Active", bundle.values.get("status"));
    assertEquals("example-symbolic", bundle.values.get("iconName"));
    assertFalse(bundle.values.containsKey("icon"));

    active.status = "Passive";
    assertNull(TrayHostItems.toBundle(null, active));
  }

  @Test
  public void passiveItemIsOmittedAndIconIsOptional() {
    TrayItem item = new TrayItem(9, "org.example.Passive", "/StatusNotifierItem", ":1.2", null);
    item.status = "Passive";
    assertFalse(TrayHostItems.writeToBundle(item, null, new RecordingBundle()));

    item.status = "NeedsAttention";
    item.icon = null;
    RecordingBundle bundle = new RecordingBundle();
    assertTrue(TrayHostItems.writeToBundle(item, null, bundle));
    assertEquals("NeedsAttention", bundle.values.get("status"));
    assertFalse(bundle.values.containsKey("icon"));
  }

  private static final class RecordingBundle implements TrayHostItems.BundleWriter {
    final Map<String, Object> values = new HashMap<>();

    @Override
    public void putString(String key, String value) {
      values.put(key, value);
    }

    @Override
    public void putParcelable(String key, Parcelable value) {
      values.put(key, value);
    }
  }
}
