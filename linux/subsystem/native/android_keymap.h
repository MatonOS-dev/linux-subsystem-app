#ifndef MATON_ANDROID_KEYMAP_H
#define MATON_ANDROID_KEYMAP_H

/* Returns a raw Linux evdev KEY_* code, or -1 if Generic.kl has no mapping.
 * No XKB +8 offset: wlroots performs that conversion internally. */
int maton_android_keycode_to_evdev(int keycode);

#endif
