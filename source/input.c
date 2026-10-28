/* input.c -- Switch controls -> GameNative input callbacks
 *
 * Button indices and axis order come from the APK's
 * com.rockstargames.oswrapper.view.InputHandler:
 *   0 A, 1 B, 2 X, 3 Y, 4 START, 6 L1, 7 R1, 8 UP, 9 DOWN, 10 LEFT, 11 RIGHT,
 *   12 L3, 13 R3, 14 BACK, 15 MENU
 *   axes: lx, ly, rx * 0.5, ry * 0.5, ltrigger, rtrigger (Android Y-down)
 * Face buttons are mapped by position (Xbox layout), like other ports.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <math.h>
#include <string.h>

#include "config.h"
#include "util.h"
#include "hooks.h"
#include "java_classes.h"

#define PAD_DEVICE_ID 1
#define STICK_DEADZONE 0.15f
#define MAX_TOUCHES 8

static PadState pad;
static u64 prev_buttons;
static float prev_axes[6];
static HidTouchScreenState prev_touch;

static const struct {
  u64 mask;
  int button;
} button_map[] = {
  { HidNpadButton_B, 0 },
  { HidNpadButton_A, 1 },
  { HidNpadButton_Y, 2 },
  { HidNpadButton_X, 3 },
  { HidNpadButton_Plus, 4 },
  { HidNpadButton_L, 6 },
  { HidNpadButton_R, 7 },
  { HidNpadButton_Up, 8 },
  { HidNpadButton_Down, 9 },
  { HidNpadButton_Left, 10 },
  { HidNpadButton_Right, 11 },
  { HidNpadButton_StickL, 12 },
  { HidNpadButton_StickR, 13 },
  { HidNpadButton_Minus, 14 },
};

void input_init(void) {
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);
  hidInitializeTouchScreen();
  natives.OnGamepadConnected(jni_env, obj_game_native_class, PAD_DEVICE_ID);
}

static void stick(HidAnalogStickState s, float *x, float *y) {
  float fx = s.x / 32767.0f;
  float fy = -s.y / 32767.0f; // Android axes are Y-down
  const float mag = sqrtf(fx * fx + fy * fy);
  if (mag < STICK_DEADZONE) {
    fx = fy = 0.0f;
  } else {
    // rescale so movement starts at 0 just outside the dead zone
    const float scale = (fminf(mag, 1.0f) - STICK_DEADZONE) / (1.0f - STICK_DEADZONE) / mag;
    fx *= scale;
    fy *= scale;
  }
  *x = fx;
  *y = fy;
}

static void update_touch(void) {
  HidTouchScreenState state = { 0 };
  if (!hidGetTouchScreenStates(&state, 1)) return;

  const float sx = (float)screen_width / 1280.0f;
  const float sy = (float)screen_height / 720.0f;

  for (int i = 0; i < state.count && i < MAX_TOUCHES; ++i) {
    const HidTouchState *t = &state.touches[i];
    int existed = 0;
    for (int j = 0; j < prev_touch.count && j < MAX_TOUCHES; ++j)
      if (prev_touch.touches[j].finger_id == t->finger_id) existed = 1;
    if (existed)
      natives.OnTouchMove(jni_env, obj_game_native_class, t->finger_id, t->x * sx, t->y * sy);
    else
      natives.OnTouchStart(jni_env, obj_game_native_class, t->finger_id, t->x * sx, t->y * sy);
  }
  for (int j = 0; j < prev_touch.count && j < MAX_TOUCHES; ++j) {
    const HidTouchState *t = &prev_touch.touches[j];
    int still = 0;
    for (int i = 0; i < state.count && i < MAX_TOUCHES; ++i)
      if (state.touches[i].finger_id == t->finger_id) still = 1;
    if (!still)
      natives.OnTouchEnd(jni_env, obj_game_native_class, t->finger_id, t->x * sx, t->y * sy);
  }
  prev_touch = state;
}

void input_update(void) {
  padUpdate(&pad);
  const u64 buttons = padGetButtons(&pad);
  const u64 changed = buttons ^ prev_buttons;

  for (size_t i = 0; i < sizeof(button_map) / sizeof(*button_map); ++i) {
    if (!(changed & button_map[i].mask)) continue;
    if (buttons & button_map[i].mask)
      natives.OnGamepadButtonDown(jni_env, obj_game_native_class, PAD_DEVICE_ID, button_map[i].button);
    else
      natives.OnGamepadButtonUp(jni_env, obj_game_native_class, PAD_DEVICE_ID, button_map[i].button);
  }
  prev_buttons = buttons;

  float axes[6];
  stick(padGetStickPos(&pad, 0), &axes[0], &axes[1]);
  stick(padGetStickPos(&pad, 1), &axes[2], &axes[3]);
  axes[2] *= 0.5f;
  axes[3] *= 0.5f;
  axes[4] = (buttons & HidNpadButton_ZL) ? 1.0f : 0.0f;
  axes[5] = (buttons & HidNpadButton_ZR) ? 1.0f : 0.0f;
  if (memcmp(axes, prev_axes, sizeof(axes))) {
    natives.OnGamepadAxesChanged(jni_env, obj_game_native_class, PAD_DEVICE_ID,
      axes[0], axes[1], axes[2], axes[3], axes[4], axes[5]);
    memcpy(prev_axes, axes, sizeof(axes));
  }

  update_touch();
}
