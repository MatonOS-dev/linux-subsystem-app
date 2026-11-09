package org.matonos.tray;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

public final class AppIconResolverTest {
  @Test
  public void choosesFallbackInRequestedPriorityOrder() {
    assertEquals(0, AppIconResolver.firstAvailable(true, true, true));
    assertEquals(1, AppIconResolver.firstAvailable(false, true, true));
    assertEquals(2, AppIconResolver.firstAvailable(false, false, true));
    assertEquals(3, AppIconResolver.firstAvailable(false, false, false));
  }

  @Test
  public void readsExactFlatpakIdMetadataWithoutPackageNameGuessing() {
    assertEquals("org.example_app.Editor", AppIconResolver.metadataFlatpakId("org.example_app.Editor"));
    assertEquals(null, AppIconResolver.metadataFlatpakId(null));
    assertEquals(null, AppIconResolver.metadataFlatpakId(""));
  }
}
