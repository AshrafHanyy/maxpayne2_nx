/* error.c -- error handler and crash reporter
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 * Crash reporter added for Max Payne Mobile 2.x.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"

void fatal_error(const char *fmt, ...) {
  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);

  va_list list;

  va_start(list, fmt);
  debugPrintf("FATAL: ");
  debugVPrintf(fmt, list);
  debugPrintf("\n");
  va_end(list);

  consoleInit(NULL);

  va_start(list, fmt);
  vprintf(fmt, list);
  va_end(list);

  printf("\n\nPress A to exit.");

  consoleUpdate(NULL);

  while (appletMainLoop()) {
    padUpdate(&pad);
    const u64 keys = padGetButtonsDown(&pad);
    if (keys & HidNpadButton_A) break;
  }

  consoleExit(NULL);
  exit(1);
}

// libnx calls this on CPU exceptions (data abort etc.) instead of crashing silently.
// Writes crash.log with registers and a frame-pointer backtrace mapped to .so offsets.

alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
u32 __nx_exception_ignoredebug = 1;

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
  char desc[256];
  FILE *f = fopen(CRASH_NAME, "w");
  if (f) {
    fprintf(f, "Max Payne crash report\nerror_desc: 0x%x\n", ctx->error_desc);
    so_describe_addr(ctx->pc.x, desc, sizeof(desc));
    fprintf(f, "pc:  %016lx %s\n", ctx->pc.x, desc);
    so_describe_addr(ctx->lr.x, desc, sizeof(desc));
    fprintf(f, "lr:  %016lx %s\n", ctx->lr.x, desc);
    fprintf(f, "sp:  %016lx\nfp:  %016lx\nfar: %016lx\nesr: %08x\n\n", ctx->sp.x, ctx->fp.x, ctx->far.x, ctx->esr);
    for (int i = 0; i < 29; ++i) {
      so_describe_addr(ctx->cpu_gprs[i].x, desc, sizeof(desc));
      fprintf(f, "x%-2d: %016lx %s\n", i, ctx->cpu_gprs[i].x, desc);
    }
    fprintf(f, "\nbacktrace (frame pointers):\n");
    uint64_t *fp = (uint64_t *)ctx->fp.x;
    for (int i = 0; i < 32 && fp && ((uintptr_t)fp & 7) == 0 && so_is_readable(fp, 16); ++i) {
      so_describe_addr(fp[1], desc, sizeof(desc));
      fprintf(f, "  #%-2d %016lx %s\n", i, fp[1], desc);
      fp = (uint64_t *)fp[0];
    }
    fclose(f);
  }
  debugPrintf("CRASH at pc=%016lx (see " CRASH_NAME ")\n", ctx->pc.x);
  svcBreak(BreakReason_Panic, 0, 0);
}
