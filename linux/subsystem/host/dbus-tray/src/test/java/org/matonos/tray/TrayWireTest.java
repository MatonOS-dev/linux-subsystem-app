package org.matonos.tray;

import static org.junit.Assert.*;

import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import org.junit.Test;
import org.matonos.dbus.DBusReader;
import org.matonos.dbus.DBusVariant;
import org.matonos.dbus.DBusWriter;

public final class TrayWireTest {
  @Test
  public void iconPixmapArrayRoundTrips() {
    List<Object> pixelBytes = Arrays.asList(255, 12, 34, 56, 128, 78, 90, 123);
    List<Object> value = Collections.singletonList(Arrays.asList(2, 1, pixelBytes));
    DBusWriter writer = new DBusWriter();
    writer.value("a(iiay)", value);
    DBusReader reader = new DBusReader(writer.toBytes());
    assertEquals(value, reader.value("a(iiay)", 0));
    assertEquals(writer.size(), reader.position());
  }

  @Test
  public void dbusMenuLayoutSignatureRoundTrips() {
    DBusVariant label = new DBusVariant("s", "Open");
    List<Object> properties = Collections.singletonList(Arrays.asList("label", label));
    List<Object> leaf = Arrays.asList(7, properties, Collections.emptyList());
    List<Object> children = Collections.singletonList(new DBusVariant("(ia{sv}av)", leaf));
    List<Object> root = Arrays.asList(0, Collections.emptyList(), children);
    DBusWriter writer = new DBusWriter();
    writer.value("(ia{sv}av)", root);
    DBusReader reader = new DBusReader(writer.toBytes());
    Object parsed = reader.value("(ia{sv}av)", 0);
    List<?> parsedRoot = (List<?>) parsed;
    assertEquals(0, parsedRoot.get(0));
    List<?> parsedChildren = (List<?>) parsedRoot.get(2);
    DBusVariant parsedLeaf = (DBusVariant) parsedChildren.get(0);
    List<?> leafNode = (List<?>) parsedLeaf.value;
    assertEquals(7, leafNode.get(0));
    List<?> pairs = (List<?>) leafNode.get(1);
    List<?> pair = (List<?>) pairs.get(0);
    assertEquals("label", pair.get(0));
    DBusVariant parsedLabel = (DBusVariant) pair.get(1);
    assertEquals("s", parsedLabel.signature);
    assertEquals("Open", parsedLabel.value);
    assertEquals(writer.size(), reader.position());
  }
}
