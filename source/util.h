/* util.h -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 * Adapted for Max Payne Mobile 2.x.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

void log_init(int enabled);
int debugPrintf(const char *text, ...) __attribute__((format(printf, 1, 2)));
int debugVPrintf(const char *text, va_list list);
FILE *log_file(void);

int ret0(void);
int ret1(void);
int retm1(void);

static inline void *armGetTlsRw(void) {
  void *ret;
  __asm__ volatile ("mrs %x[data], tpidr_el0" : [data] "=r" (ret));
  return ret;
}

static inline void armSetTlsRw(void *addr) {
  __asm__ volatile ("msr tpidr_el0, %0" : : "r"(addr));
}

static inline uint64_t umin(uint64_t a, uint64_t b) {
  return (a < b) ? a : b;
}

#endif
