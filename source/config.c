/* config.c -- simple configuration parser
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 * Adapted for Max Payne Mobile 2.x.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "config.h"

#define CONFIG_VARS \
  CONFIG_VAR_INT(screen_width); \
  CONFIG_VAR_INT(screen_height); \
  CONFIG_VAR_INT(trilinear_filter); \
  CONFIG_VAR_INT(disable_mipmaps); \
  CONFIG_VAR_INT(language); \
  CONFIG_VAR_INT(crouch_toggle); \
  CONFIG_VAR_INT(character_shadows); \
  CONFIG_VAR_INT(drop_highest_lod); \
  CONFIG_VAR_INT(show_weapon_menu); \
  CONFIG_VAR_FLOAT(decal_limit); \
  CONFIG_VAR_FLOAT(debris_limit); \
  CONFIG_VAR_INT(touch_screen); \
  CONFIG_VAR_INT(debug_log); \
  CONFIG_VAR_INT(gl_trace);

Config config;

// actual screen size that is in use right now
int screen_width = 1280;
int screen_height = 720;

static inline void parse_var(const char *name, const char *value) {
  #define CONFIG_VAR_INT(var) if (!strcmp(name, #var)) { config.var = atoi(value); return; }
  #define CONFIG_VAR_FLOAT(var) if (!strcmp(name, #var)) { config.var = atof(value); return; }
  CONFIG_VARS
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_FLOAT
}

int read_config(const char *file) {
  char line[1024] = { 0 };

  memset(&config, 0, sizeof(Config));
  config.screen_width = -1; // auto
  config.screen_height = -1;
  config.trilinear_filter = 1;
  config.disable_mipmaps = 0;
  config.language = 0; // english
  config.crouch_toggle = 1;
  config.character_shadows = 1; // 1 - one blob; 2 - foot shadows
  config.drop_highest_lod = 0;
  config.show_weapon_menu = 0;
  config.decal_limit = 0.5f;
  config.debris_limit = 1.0f;
  config.touch_screen = 0; // report a touch screen to the game (shows touch controls)
  config.debug_log = 1;
  config.gl_trace = 0; // check every GL call for errors + one-frame trace (slow)

  FILE *f = fopen(file, "r");
  if (f == NULL)
    return -1;

  // parse lines of the forms
  // <spaces> # <whatever> \n
  // <spaces> NAME <spaces> VALUE <spaces> \n
  do {
    char *name = NULL, *value = NULL, *tmp = NULL;
    if (fgets(line, sizeof(line), f) != NULL) {
      name = line;
      // trim name
      while (*name && isspace((int)*name)) ++name;
      if (name[0] == '#') continue; // skip comments
      for (tmp = name; *tmp && !isspace((int)*tmp); ++tmp);
      // if tmp points to the end of the string, there's no value to parse
      if (*tmp != 0) {
        *tmp = 0;
        // value is next; trim value
        for (value = tmp + 1; *value && isspace((int)*value); ++value);
        for (tmp = value + strlen(value) - 1; tmp >= value && isspace((int)*tmp); --tmp) *tmp = 0;
        // got key value pair
        parse_var(name, value);
      }
    }
  } while (!feof(f));

  fclose(f);

  return 0;
}

int write_config(const char *file) {
  FILE *f = fopen(file, "w");
  if (f == NULL)
    return -1;

  #define CONFIG_VAR_INT(var) fprintf(f, "%s %d\n", #var, config.var)
  #define CONFIG_VAR_FLOAT(var) fprintf(f, "%s %g\n", #var, config.var)
  CONFIG_VARS
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_FLOAT

  fclose(f);

  return 0;
}
