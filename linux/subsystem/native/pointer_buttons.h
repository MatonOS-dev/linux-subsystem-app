#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef void (*MatonButtonNotify)(void *data, uint32_t code, bool pressed);

/* Android primary, secondary, tertiary, back and forward map to evdev buttons.
 * Emit edges only: motion and duplicate Android button actions cannot click. */
static inline void maton_pointer_buttons_update(uint32_t *previous, uint32_t buttons,
                                                MatonButtonNotify notify, void *data) {
  static const uint32_t codes[] = {0x110, 0x111, 0x112, 0x116, 0x115};
  buttons &= 31;
  uint32_t changed = *previous ^ buttons;
  for (unsigned i = 0; i < 5; ++i) {
    uint32_t mask = 1u << i;
    if (changed & mask) notify(data, codes[i], (buttons & mask) != 0);
  }
  *previous = buttons;
}
