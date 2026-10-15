package org.matonos.linux;

import org.junit.Test;
import static org.junit.Assert.assertEquals;

public class PointerInputTest {
    @Test public void mouseMotionDoesNotHoldLeftButton() {
        assertEquals(0, PointerInput.buttons(7, 0, false));
        assertEquals(1, PointerInput.buttons(0, 1, false));
        assertEquals(1, PointerInput.buttons(11, 1, false));
        assertEquals(1, PointerInput.buttons(2, 1, false));
        assertEquals(0, PointerInput.buttons(1, 0, false));
        assertEquals(0, PointerInput.buttons(12, 0, false));
        assertEquals(0, PointerInput.buttons(2, 0, false));
    }
    @Test public void chordsUseActualButtonState() {
        assertEquals(2, PointerInput.buttons(11, 2, false));
        assertEquals(3, PointerInput.buttons(11, 3, false));
        assertEquals(2, PointerInput.buttons(12, 2, false));
        assertEquals(4, PointerInput.buttons(2, 4, false));
    }
    @Test public void touchscreenContactActsAsLeftButton() {
        assertEquals(1, PointerInput.buttons(0, 0, true));
        assertEquals(1, PointerInput.buttons(2, 0, true));
        assertEquals(1, PointerInput.buttons(6, 0, true));
        assertEquals(0, PointerInput.buttons(1, 0, true));
    }
    @Test public void cancelAndExitReleaseButtons() {
        assertEquals(0, PointerInput.buttons(3, 3, false));
        assertEquals(0, PointerInput.buttons(10, 1, false));
        assertEquals(0, PointerInput.buttons(3, 0, true));
    }
}
