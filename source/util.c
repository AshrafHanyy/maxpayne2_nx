/* util.c -- misc utility functions and logging
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 * Adapted for Max Payne Mobile 2.x.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "config.h"

static FILE *s_log = NULL;
static Mutex s_log_lock;

void log_init(int enabled) {
  if (!enabled) return;
  s_log = fopen(LOG_NAME, "w");
}

FILE *log_file(void) {
  return s_log;
}

int debugVPrintf(const char *text, va_list list) {
  if (!s_log) return 0;
  mutexLock(&s_log_lock);
  vfprintf(s_log, text, list);
  fflush(s_log);
  mutexUnlock(&s_log_lock);
  return 0;
}

int debugPrintf(const char *text, ...) {
  va_list list;
  va_start(list, text);
  debugVPrintf(text, list);
  va_end(list);
  return 0;
}

int ret0(void) { return 0; }

int ret1(void) { return 1; }

int retm1(void) { return -1; }
