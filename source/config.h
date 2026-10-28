/* config.h -- global configuration and config file handling
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 * Adapted for Max Payne Mobile 2.x.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// memory reserved at the top of the heap for the loaded .so modules
#define SO_REGION_MB 64

#define GAME_SO_NAME "libGame.so"
#define CXX_SO_NAME "libc++_shared.so"
#define CONFIG_NAME "config.txt"
#define LOG_NAME "debug.log"
#define CRASH_NAME "crash.log"

#define DEFAULT_DATA_DIR "sdmc:/switch/maxpayne"

// actual screen size
extern int screen_width;
extern int screen_height;

typedef struct {
  int screen_width;
  int screen_height;
  int trilinear_filter;
  int disable_mipmaps;
  int language;
  int crouch_toggle;
  int character_shadows;
  int drop_highest_lod;
  int show_weapon_menu;
  float decal_limit;
  float debris_limit;
  int touch_screen;
  int debug_log;
  int gl_trace;
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
