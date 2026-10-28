/* hooks.c -- patches to libGame.so
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 * Adapted for Max Payne Mobile 2.x.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>

#include "config.h"
#include "util.h"
#include "so_util.h"
#include "hooks.h"

// control binding array (32 entries of 56 bytes, same as 1.7)
typedef struct {
  int unk[14];
} MaxPayne_InputControl;

static MaxPayne_InputControl *sm_control = NULL;
static int (*MaxPayne_InputControl_getButton)(MaxPayne_InputControl *, int);

static int MaxPayne_ConfiguredInput_readCrouch(void *this) {
  static int prev = 0;
  static int latch = 0;
  // crouch is control #5
  const int new = MaxPayne_InputControl_getButton(&sm_control[5], 0);
  if (prev != new) {
    prev = new;
    if (new) latch = !latch;
  }
  return latch;
}

static int X_DetailLevel_getCharacterShadows(void) {
  return config.character_shadows;
}

static int X_DetailLevel_getDropHighestLOD(void) {
  return config.drop_highest_lod;
}

static float X_DetailLevel_getDecalLimitMultiplier(void) {
  return config.decal_limit;
}

static float X_DetailLevel_getDebrisProjectileLimitMultiplier(void) {
  return config.debris_limit;
}

void patch_game(so_module *game) {
  // Social Club cloud/telemetry needs network services we don't have
  so_patch_ret(game, "_Z25OS_IsRockstarCloudEnabledv", 0);

  // sets CPU affinity through raw Linux syscalls
  so_hook(game, "_Z29AND_CurrentThreadAffinityMaskPKci", (uintptr_t)ret0);

  so_hook(game, "_Z14Mobile_Vibratei", (uintptr_t)ret0);

  // detail level getters -> our settings
  so_hook(game, "_ZN13X_DetailLevel19getCharacterShadowsEv", (uintptr_t)X_DetailLevel_getCharacterShadows);
  so_hook(game, "_ZN13X_DetailLevel34getDebrisProjectileLimitMultiplierEv", (uintptr_t)X_DetailLevel_getDebrisProjectileLimitMultiplier);
  so_hook(game, "_ZN13X_DetailLevel23getDecalLimitMultiplierEv", (uintptr_t)X_DetailLevel_getDecalLimitMultiplier);
  so_hook(game, "_ZN13X_DetailLevel13dropHighesLODEv", (uintptr_t)X_DetailLevel_getDropHighestLOD);

  // NOTE: don't patch UseBloom() like max_nx did. In 2.x it always returns 1 and
  // StopSceneFill always runs the bloom pipeline (which also draws the final
  // image), so returning 0 leaves the bloom programs uncreated and the 3D view black.

  // dummy out the weapon menu arrow drawer if it's disabled
  if (!config.show_weapon_menu)
    so_hook(game, "_ZN12WeaponSwiper4DrawEv", (uintptr_t)ret0);

  if (config.crouch_toggle) {
    sm_control = (void *)so_symbol(game, "_ZN24MaxPayne_ConfiguredInput10sm_controlE");
    MaxPayne_InputControl_getButton = (void *)so_symbol(game, "_ZNK21MaxPayne_InputControl9getButtonEi");
    so_hook(game, "_ZNK24MaxPayne_ConfiguredInput10readCrouchEv", (uintptr_t)MaxPayne_ConfiguredInput_readCrouch);
  }
}

void patch_game_data(so_module *game) {
  // RGB565 FBOs suck
  *(uint8_t *)so_symbol(game, "UseRGBA8") = 1;
}
