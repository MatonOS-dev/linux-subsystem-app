/* Generated from AOSP Generic.kl and android/keycodes.h.
 * Generic.kl is Copyright (C) 2010 The Android Open Source Project,
 * licensed under Apache-2.0 (https://www.apache.org/licenses/LICENSE-2.0).
 * Regenerate: python3 native/tools/generate_android_keymap.py AOSP_ROOT
 * Numeric evdev mappings only; first mapping wins for duplicate keycodes.
 * HID usage entries cannot supply an evdev code and are excluded.
 */
#include "android_keymap.h"
#include <stddef.h>

static const struct { int android; int evdev; } keymap[] = {
  { 3, 172 }, /* KEYCODE_HOME */
  { 4, 158 }, /* KEYCODE_BACK */
  { 5, 169 }, /* KEYCODE_CALL */
  { 7, 11 }, /* KEYCODE_0 */
  { 8, 2 }, /* KEYCODE_1 */
  { 9, 3 }, /* KEYCODE_2 */
  { 10, 4 }, /* KEYCODE_3 */
  { 11, 5 }, /* KEYCODE_4 */
  { 12, 6 }, /* KEYCODE_5 */
  { 13, 7 }, /* KEYCODE_6 */
  { 14, 8 }, /* KEYCODE_7 */
  { 15, 9 }, /* KEYCODE_8 */
  { 16, 10 }, /* KEYCODE_9 */
  { 17, 522 }, /* KEYCODE_STAR */
  { 18, 523 }, /* KEYCODE_POUND */
  { 19, 103 }, /* KEYCODE_DPAD_UP */
  { 20, 108 }, /* KEYCODE_DPAD_DOWN */
  { 21, 105 }, /* KEYCODE_DPAD_LEFT */
  { 22, 106 }, /* KEYCODE_DPAD_RIGHT */
  { 23, 353 }, /* KEYCODE_DPAD_CENTER */
  { 24, 115 }, /* KEYCODE_VOLUME_UP */
  { 25, 114 }, /* KEYCODE_VOLUME_DOWN */
  { 26, 116 }, /* KEYCODE_POWER */
  { 27, 212 }, /* KEYCODE_CAMERA */
  { 29, 30 }, /* KEYCODE_A */
  { 30, 48 }, /* KEYCODE_B */
  { 31, 46 }, /* KEYCODE_C */
  { 32, 32 }, /* KEYCODE_D */
  { 33, 18 }, /* KEYCODE_E */
  { 34, 33 }, /* KEYCODE_F */
  { 35, 34 }, /* KEYCODE_G */
  { 36, 35 }, /* KEYCODE_H */
  { 37, 23 }, /* KEYCODE_I */
  { 38, 36 }, /* KEYCODE_J */
  { 39, 37 }, /* KEYCODE_K */
  { 40, 38 }, /* KEYCODE_L */
  { 41, 50 }, /* KEYCODE_M */
  { 42, 49 }, /* KEYCODE_N */
  { 43, 24 }, /* KEYCODE_O */
  { 44, 25 }, /* KEYCODE_P */
  { 45, 16 }, /* KEYCODE_Q */
  { 46, 19 }, /* KEYCODE_R */
  { 47, 31 }, /* KEYCODE_S */
  { 48, 20 }, /* KEYCODE_T */
  { 49, 22 }, /* KEYCODE_U */
  { 50, 47 }, /* KEYCODE_V */
  { 51, 17 }, /* KEYCODE_W */
  { 52, 45 }, /* KEYCODE_X */
  { 53, 21 }, /* KEYCODE_Y */
  { 54, 44 }, /* KEYCODE_Z */
  { 55, 51 }, /* KEYCODE_COMMA */
  { 56, 52 }, /* KEYCODE_PERIOD */
  { 57, 56 }, /* KEYCODE_ALT_LEFT */
  { 58, 100 }, /* KEYCODE_ALT_RIGHT */
  { 59, 42 }, /* KEYCODE_SHIFT_LEFT */
  { 60, 54 }, /* KEYCODE_SHIFT_RIGHT */
  { 61, 15 }, /* KEYCODE_TAB */
  { 62, 57 }, /* KEYCODE_SPACE */
  { 64, 150 }, /* KEYCODE_EXPLORER */
  { 65, 155 }, /* KEYCODE_ENVELOPE */
  { 66, 28 }, /* KEYCODE_ENTER */
  { 67, 14 }, /* KEYCODE_DEL */
  { 68, 41 }, /* KEYCODE_GRAVE */
  { 69, 12 }, /* KEYCODE_MINUS */
  { 70, 13 }, /* KEYCODE_EQUALS */
  { 71, 26 }, /* KEYCODE_LEFT_BRACKET */
  { 72, 27 }, /* KEYCODE_RIGHT_BRACKET */
  { 73, 43 }, /* KEYCODE_BACKSLASH */
  { 74, 39 }, /* KEYCODE_SEMICOLON */
  { 75, 40 }, /* KEYCODE_APOSTROPHE */
  { 76, 53 }, /* KEYCODE_SLASH */
  { 79, 226 }, /* KEYCODE_HEADSETHOOK */
  { 80, 528 }, /* KEYCODE_FOCUS */
  { 82, 127 }, /* KEYCODE_MENU */
  { 83, 204 }, /* KEYCODE_NOTIFICATION */
  { 84, 217 }, /* KEYCODE_SEARCH */
  { 85, 164 }, /* KEYCODE_MEDIA_PLAY_PAUSE */
  { 86, 128 }, /* KEYCODE_MEDIA_STOP */
  { 87, 163 }, /* KEYCODE_MEDIA_NEXT */
  { 88, 165 }, /* KEYCODE_MEDIA_PREVIOUS */
  { 89, 168 }, /* KEYCODE_MEDIA_REWIND */
  { 90, 208 }, /* KEYCODE_MEDIA_FAST_FORWARD */
  { 91, 248 }, /* KEYCODE_MUTE */
  { 92, 104 }, /* KEYCODE_PAGE_UP */
  { 93, 109 }, /* KEYCODE_PAGE_DOWN */
  { 96, 304 }, /* KEYCODE_BUTTON_A */
  { 97, 305 }, /* KEYCODE_BUTTON_B */
  { 98, 306 }, /* KEYCODE_BUTTON_C */
  { 99, 307 }, /* KEYCODE_BUTTON_X */
  { 100, 308 }, /* KEYCODE_BUTTON_Y */
  { 101, 309 }, /* KEYCODE_BUTTON_Z */
  { 102, 310 }, /* KEYCODE_BUTTON_L1 */
  { 103, 311 }, /* KEYCODE_BUTTON_R1 */
  { 104, 312 }, /* KEYCODE_BUTTON_L2 */
  { 105, 313 }, /* KEYCODE_BUTTON_R2 */
  { 106, 317 }, /* KEYCODE_BUTTON_THUMBL */
  { 107, 318 }, /* KEYCODE_BUTTON_THUMBR */
  { 108, 315 }, /* KEYCODE_BUTTON_START */
  { 109, 314 }, /* KEYCODE_BUTTON_SELECT */
  { 110, 316 }, /* KEYCODE_BUTTON_MODE */
  { 111, 1 }, /* KEYCODE_ESCAPE */
  { 112, 111 }, /* KEYCODE_FORWARD_DEL */
  { 113, 29 }, /* KEYCODE_CTRL_LEFT */
  { 114, 97 }, /* KEYCODE_CTRL_RIGHT */
  { 115, 58 }, /* KEYCODE_CAPS_LOCK */
  { 116, 70 }, /* KEYCODE_SCROLL_LOCK */
  { 117, 125 }, /* KEYCODE_META_LEFT */
  { 118, 126 }, /* KEYCODE_META_RIGHT */
  { 119, 464 }, /* KEYCODE_FUNCTION */
  { 120, 99 }, /* KEYCODE_SYSRQ */
  { 121, 119 }, /* KEYCODE_BREAK */
  { 122, 102 }, /* KEYCODE_MOVE_HOME */
  { 123, 107 }, /* KEYCODE_MOVE_END */
  { 124, 110 }, /* KEYCODE_INSERT */
  { 125, 159 }, /* KEYCODE_FORWARD */
  { 126, 200 }, /* KEYCODE_MEDIA_PLAY */
  { 127, 201 }, /* KEYCODE_MEDIA_PAUSE */
  { 128, 160 }, /* KEYCODE_MEDIA_CLOSE */
  { 129, 161 }, /* KEYCODE_MEDIA_EJECT */
  { 130, 167 }, /* KEYCODE_MEDIA_RECORD */
  { 131, 59 }, /* KEYCODE_F1 */
  { 132, 60 }, /* KEYCODE_F2 */
  { 133, 61 }, /* KEYCODE_F3 */
  { 134, 62 }, /* KEYCODE_F4 */
  { 135, 63 }, /* KEYCODE_F5 */
  { 136, 64 }, /* KEYCODE_F6 */
  { 137, 65 }, /* KEYCODE_F7 */
  { 138, 66 }, /* KEYCODE_F8 */
  { 139, 67 }, /* KEYCODE_F9 */
  { 140, 68 }, /* KEYCODE_F10 */
  { 141, 87 }, /* KEYCODE_F11 */
  { 142, 88 }, /* KEYCODE_F12 */
  { 143, 69 }, /* KEYCODE_NUM_LOCK */
  { 144, 82 }, /* KEYCODE_NUMPAD_0 */
  { 145, 79 }, /* KEYCODE_NUMPAD_1 */
  { 146, 80 }, /* KEYCODE_NUMPAD_2 */
  { 147, 81 }, /* KEYCODE_NUMPAD_3 */
  { 148, 75 }, /* KEYCODE_NUMPAD_4 */
  { 149, 76 }, /* KEYCODE_NUMPAD_5 */
  { 150, 77 }, /* KEYCODE_NUMPAD_6 */
  { 151, 71 }, /* KEYCODE_NUMPAD_7 */
  { 152, 72 }, /* KEYCODE_NUMPAD_8 */
  { 153, 73 }, /* KEYCODE_NUMPAD_9 */
  { 154, 98 }, /* KEYCODE_NUMPAD_DIVIDE */
  { 155, 55 }, /* KEYCODE_NUMPAD_MULTIPLY */
  { 156, 74 }, /* KEYCODE_NUMPAD_SUBTRACT */
  { 157, 78 }, /* KEYCODE_NUMPAD_ADD */
  { 158, 83 }, /* KEYCODE_NUMPAD_DOT */
  { 159, 95 }, /* KEYCODE_NUMPAD_COMMA */
  { 160, 96 }, /* KEYCODE_NUMPAD_ENTER */
  { 161, 117 }, /* KEYCODE_NUMPAD_EQUALS */
  { 162, 179 }, /* KEYCODE_NUMPAD_LEFT_PAREN */
  { 163, 180 }, /* KEYCODE_NUMPAD_RIGHT_PAREN */
  { 164, 113 }, /* KEYCODE_VOLUME_MUTE */
  { 166, 402 }, /* KEYCODE_CHANNEL_UP */
  { 167, 403 }, /* KEYCODE_CHANNEL_DOWN */
  { 168, 418 }, /* KEYCODE_ZOOM_IN */
  { 169, 419 }, /* KEYCODE_ZOOM_OUT */
  { 170, 377 }, /* KEYCODE_TV */
  { 172, 362 }, /* KEYCODE_GUIDE */
  { 173, 366 }, /* KEYCODE_DVR */
  { 174, 156 }, /* KEYCODE_BOOKMARK */
  { 175, 370 }, /* KEYCODE_CAPTIONS */
  { 183, 398 }, /* KEYCODE_PROG_RED */
  { 184, 399 }, /* KEYCODE_PROG_GREEN */
  { 185, 400 }, /* KEYCODE_PROG_YELLOW */
  { 186, 401 }, /* KEYCODE_PROG_BLUE */
  { 187, 580 }, /* KEYCODE_APP_SWITCH */
  { 188, 256 }, /* KEYCODE_BUTTON_1 */
  { 189, 257 }, /* KEYCODE_BUTTON_2 */
  { 190, 258 }, /* KEYCODE_BUTTON_3 */
  { 191, 259 }, /* KEYCODE_BUTTON_4 */
  { 192, 260 }, /* KEYCODE_BUTTON_5 */
  { 193, 261 }, /* KEYCODE_BUTTON_6 */
  { 194, 262 }, /* KEYCODE_BUTTON_7 */
  { 195, 263 }, /* KEYCODE_BUTTON_8 */
  { 196, 264 }, /* KEYCODE_BUTTON_9 */
  { 197, 265 }, /* KEYCODE_BUTTON_10 */
  { 198, 266 }, /* KEYCODE_BUTTON_11 */
  { 199, 267 }, /* KEYCODE_BUTTON_12 */
  { 200, 268 }, /* KEYCODE_BUTTON_13 */
  { 201, 269 }, /* KEYCODE_BUTTON_14 */
  { 202, 270 }, /* KEYCODE_BUTTON_15 */
  { 203, 271 }, /* KEYCODE_BUTTON_16 */
  { 204, 368 }, /* KEYCODE_LANGUAGE_SWITCH */
  { 207, 429 }, /* KEYCODE_CONTACTS */
  { 208, 397 }, /* KEYCODE_CALENDAR */
  { 209, 171 }, /* KEYCODE_MUSIC */
  { 210, 140 }, /* KEYCODE_CALCULATOR */
  { 211, 85 }, /* KEYCODE_ZENKAKU_HANKAKU */
  { 212, 123 }, /* KEYCODE_EISU */
  { 213, 94 }, /* KEYCODE_MUHENKAN */
  { 214, 92 }, /* KEYCODE_HENKAN */
  { 215, 93 }, /* KEYCODE_KATAKANA_HIRAGANA */
  { 216, 124 }, /* KEYCODE_YEN */
  { 217, 89 }, /* KEYCODE_RO */
  { 218, 122 }, /* KEYCODE_KANA */
  { 219, 583 }, /* KEYCODE_ASSIST */
  { 220, 224 }, /* KEYCODE_BRIGHTNESS_DOWN */
  { 221, 225 }, /* KEYCODE_BRIGHTNESS_UP */
  { 223, 142 }, /* KEYCODE_SLEEP */
  { 224, 143 }, /* KEYCODE_WAKEUP */
  { 229, 405 }, /* KEYCODE_LAST_CHANNEL */
  { 231, 582 }, /* KEYCODE_VOICE_ASSIST */
  { 277, 137 }, /* KEYCODE_CUT */
  { 278, 133 }, /* KEYCODE_COPY */
  { 279, 135 }, /* KEYCODE_PASTE */
  { 285, 173 }, /* KEYCODE_REFRESH */
  { 305, 229 }, /* KEYCODE_KEYBOARD_BACKLIGHT_DOWN */
  { 306, 230 }, /* KEYCODE_KEYBOARD_BACKLIGHT_UP */
  { 307, 228 }, /* KEYCODE_KEYBOARD_BACKLIGHT_TOGGLE */
  { 308, 331 }, /* KEYCODE_STYLUS_BUTTON_PRIMARY */
  { 309, 332 }, /* KEYCODE_STYLUS_BUTTON_SECONDARY */
  { 310, 329 }, /* KEYCODE_STYLUS_BUTTON_TERTIARY */
  { 312, 120 }, /* KEYCODE_RECENT_APPS */
  { 313, 656 }, /* KEYCODE_MACRO_1 */
  { 314, 657 }, /* KEYCODE_MACRO_2 */
  { 315, 658 }, /* KEYCODE_MACRO_3 */
  { 316, 659 }, /* KEYCODE_MACRO_4 */
  { 317, 585 }, /* KEYCODE_EMOJI_PICKER */
  { 319, 586 }, /* KEYCODE_DICTATE */
  { 320, 181 }, /* KEYCODE_NEW */
  { 321, 206 }, /* KEYCODE_CLOSE */
  { 322, 591 }, /* KEYCODE_DO_NOT_DISTURB */
  { 323, 210 }, /* KEYCODE_PRINT */
  { 324, 152 }, /* KEYCODE_LOCK */
  { 325, 372 }, /* KEYCODE_FULLSCREEN */
  { 326, 183 }, /* KEYCODE_F13 */
  { 327, 184 }, /* KEYCODE_F14 */
  { 328, 185 }, /* KEYCODE_F15 */
  { 329, 186 }, /* KEYCODE_F16 */
  { 330, 187 }, /* KEYCODE_F17 */
  { 331, 188 }, /* KEYCODE_F18 */
  { 332, 189 }, /* KEYCODE_F19 */
  { 333, 190 }, /* KEYCODE_F20 */
  { 334, 191 }, /* KEYCODE_F21 */
  { 335, 192 }, /* KEYCODE_F22 */
  { 336, 193 }, /* KEYCODE_F23 */
  { 337, 194 }, /* KEYCODE_F24 */
  { 338, 590 }, /* KEYCODE_ACCESSIBILITY */
};

int maton_android_keycode_to_evdev(int keycode) {
  for (size_t i = 0; i < sizeof(keymap) / sizeof(keymap[0]); ++i)
    if (keymap[i].android == keycode) return keymap[i].evdev;
  return -1;
}

/* Duplicate choices in Generic.kl file order:
 * BACKSLASH: keep 43, ignore 86
 * NUMPAD_COMMA: keep 95, ignore 121
 * MENU: keep 127, ignore 139
 * MEDIA_EJECT: keep 161, ignore 162
 * MEDIA_STOP: keep 128, ignore 166
 * PAGE_UP: keep 104, ignore 177
 * PAGE_DOWN: keep 109, ignore 178
 * MEDIA_PLAY: keep 200, ignore 207
 * MUSIC: keep 171, ignore 213
 * ENVELOPE: keep 155, ignore 215
 * BUTTON_1: keep 256, ignore 288
 * BUTTON_2: keep 257, ignore 289
 * BUTTON_3: keep 258, ignore 290
 * BUTTON_4: keep 259, ignore 291
 * BUTTON_5: keep 260, ignore 292
 * BUTTON_6: keep 261, ignore 293
 * BUTTON_7: keep 262, ignore 294
 * BUTTON_8: keep 263, ignore 295
 * BUTTON_9: keep 264, ignore 296
 * BUTTON_10: keep 265, ignore 297
 * BUTTON_11: keep 266, ignore 298
 * BUTTON_12: keep 267, ignore 299
 * BUTTON_13: keep 268, ignore 300
 * BUTTON_14: keep 269, ignore 301
 * BUTTON_15: keep 270, ignore 302
 * BUTTON_16: keep 271, ignore 303
 * ESCAPE: keep 1, ignore 465
 * F1: keep 59, ignore 466
 * F2: keep 60, ignore 467
 * F3: keep 61, ignore 468
 * F4: keep 62, ignore 469
 * F5: keep 63, ignore 470
 * F6: keep 64, ignore 471
 * F7: keep 65, ignore 472
 * F8: keep 66, ignore 473
 * F9: keep 67, ignore 474
 * F10: keep 68, ignore 475
 * F11: keep 87, ignore 476
 * F12: keep 88, ignore 477
 * 1: keep 2, ignore 478
 * 2: keep 3, ignore 479
 * D: keep 32, ignore 480
 * E: keep 18, ignore 481
 * F: keep 33, ignore 482
 * S: keep 31, ignore 483
 * B: keep 48, ignore 484
 */
