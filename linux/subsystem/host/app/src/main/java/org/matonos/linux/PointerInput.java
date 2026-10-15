package org.matonos.linux;

/** Convert Android contact input to pointer buttons; mouse buttons are snapshots. */
public final class PointerInput {
    private PointerInput() {}

    public static int buttons(int action, int buttons, boolean touchscreen) {
        // Cancel and hover exit must end any pending pointer grab.
        if (action == 3 || action == 10) return 0;
        if (touchscreen) {
            switch (action) {
                case 0: // ACTION_DOWN
                case 2: // ACTION_MOVE
                case 5: // ACTION_POINTER_DOWN
                case 6: // ACTION_POINTER_UP (another contact remains)
                    return buttons | 1;
                case 1: // ACTION_UP
                    return 0;
            }
        }
        return buttons;
    }
}
