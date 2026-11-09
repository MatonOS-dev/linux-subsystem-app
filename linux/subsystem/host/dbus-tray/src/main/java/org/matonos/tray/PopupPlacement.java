package org.matonos.tray;

/** Screen placement for the tray popup, kept independent for JVM coverage. */
final class PopupPlacement {
  private PopupPlacement() {}

  static int[] place(int screenWidth, int screenHeight, int anchorLeft, int anchorTop,
      int anchorBottom, int popupWidth, int popupHeight) {
    int width = Math.max(1, Math.min(popupWidth, screenWidth));
    int height = Math.max(1, Math.min(popupHeight, screenHeight));
    int left = Math.max(0, Math.min(anchorLeft, screenWidth - width));
    int top = anchorTop - height;
    if (top < 0) {
      top = anchorBottom;
    }
    top = Math.max(0, Math.min(top, screenHeight - height));
    return new int[] {left, top, width, height};
  }
}
