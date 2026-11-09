package org.matonos.tray;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

public final class PopupPlacementTest {
  @Test
  public void alignsLeftEdgeAndOpensAboveAnchor() {
    int[] result = PopupPlacement.place(1000, 800, 240, 720, 776, 240, 300);
    assertPlacement(240, 420, 240, 300, result);
  }

  @Test
  public void clampsHorizontallyAndFallsBelowWhenTopWouldBeOffscreen() {
    int[] result = PopupPlacement.place(500, 400, 480, 20, 64, 240, 180);
    assertPlacement(260, 64, 240, 180, result);
  }

  @Test
  public void clampsPopupLargerThanScreen() {
    int[] result = PopupPlacement.place(200, 100, -5, 90, 100, 240, 180);
    assertPlacement(0, 0, 200, 100, result);
  }

  private void assertPlacement(int left, int top, int width, int height, int[] actual) {
    assertEquals(left, actual[0]);
    assertEquals(top, actual[1]);
    assertEquals(width, actual[2]);
    assertEquals(height, actual[3]);
  }
}
