#include "../android_keymap.h"
#include <assert.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <stdio.h>

int main(void) {
  assert(maton_android_keycode_to_evdev(20) == KEY_DOWN);
  assert(maton_android_keycode_to_evdev(19) == KEY_UP);
  assert(maton_android_keycode_to_evdev(21) == KEY_LEFT);
  assert(maton_android_keycode_to_evdev(22) == KEY_RIGHT);
  assert(maton_android_keycode_to_evdev(29) == KEY_A);
  assert(maton_android_keycode_to_evdev(66) == KEY_ENTER);
  assert(maton_android_keycode_to_evdev(67) == KEY_BACKSPACE);
  assert(maton_android_keycode_to_evdev(59) == KEY_LEFTSHIFT);
  assert(maton_android_keycode_to_evdev(113) == KEY_LEFTCTRL);
  assert(maton_android_keycode_to_evdev(111) == KEY_ESC);
  assert(maton_android_keycode_to_evdev(188) == 0x100); /* BUTTON_1 first, not 288 */
  assert(maton_android_keycode_to_evdev(0) == -1);
  assert(maton_android_keycode_to_evdev(-1) == -1);
  assert(maton_android_keycode_to_evdev(INT_MAX) == -1);
  puts("Android keycode fallback passed");
}
