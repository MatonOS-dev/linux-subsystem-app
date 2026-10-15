#include "../pointer_buttons.h"
#include <assert.h>
#include <stdio.h>

static struct { uint32_t code; bool pressed; } events[32];
static unsigned count;
static void record(void* data, uint32_t code, bool pressed) {
  (void)data;
  assert(count < 32);
  events[count].code = code;
  events[count++].pressed = pressed;
}
int main(void) {
  uint32_t state = 0;
  // Hover, press, drag, repeated button action, release, then free motion.
  maton_pointer_buttons_update(&state, 0, record, NULL);
  assert(count == 0);
  maton_pointer_buttons_update(&state, 1, record, NULL);
  maton_pointer_buttons_update(&state, 1, record, NULL);
  maton_pointer_buttons_update(&state, 1, record, NULL);
  assert(count == 1 && events[0].code == 0x110 && events[0].pressed);
  maton_pointer_buttons_update(&state, 0, record, NULL);
  maton_pointer_buttons_update(&state, 0, record, NULL);
  assert(count == 2 && events[1].code == 0x110 && !events[1].pressed);
  // Right and middle chords retain other held buttons when one releases.
  maton_pointer_buttons_update(&state, 2, record, NULL);
  maton_pointer_buttons_update(&state, 6, record, NULL);
  maton_pointer_buttons_update(&state, 4, record, NULL);
  assert(count == 5 && state == 4);
  assert(events[2].code == 0x111 && events[2].pressed);
  assert(events[3].code == 0x112 && events[3].pressed);
  assert(events[4].code == 0x111 && !events[4].pressed);
  maton_pointer_buttons_update(&state, 0, record, NULL);
  assert(count == 6 && !events[5].pressed);
  // Cancellation of a chord releases every button once; unknown bits ignored.
  maton_pointer_buttons_update(&state, 31 | 128, record, NULL);
  assert(count == 11 && state == 31);
  assert(events[9].code == 0x116 && events[10].code == 0x115);
  maton_pointer_buttons_update(&state, 0, record, NULL);
  assert(count == 16 && state == 0);
  for (unsigned i = 11; i < 16; ++i) assert(!events[i].pressed);
  puts("Pointer button sequences passed");
}
