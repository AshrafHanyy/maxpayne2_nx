/* so_util.h -- multi-module loader for Android AArch64 .so files
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 * Rewritten for multiple modules / RELR / multi-segment ELFs.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __SO_UTIL_H__
#define __SO_UTIL_H__

#include <stdint.h>
#include <stddef.h>

#define ALIGN_MEM(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

typedef struct {
  const char *symbol;
  uintptr_t func;
} DynLibFunction;

typedef struct so_module so_module;

// memory region the modules are staged in before being mapped as code
void so_init_region(void *base, size_t size);

// table of symbols we provide to the modules (checked before other modules)
void so_set_imports(const DynLibFunction *funcs, size_t num);

so_module *so_load(const char *filename);
void so_relocate(so_module *mod);
void so_finalize(so_module *mod);
void so_run_init(so_module *mod);

// address of a symbol as seen by running code (valid after so_finalize)
uintptr_t so_symbol(so_module *mod, const char *name);
// same, but returns 0 instead of failing when missing
uintptr_t so_symbol_opt(so_module *mod, const char *name);

// patching; only valid before so_finalize
void so_hook(so_module *mod, const char *name, uintptr_t dst);
void so_patch_ret(so_module *mod, const char *name, uint32_t value);

void so_describe_addr(uintptr_t addr, char *out, size_t outsize);
int so_is_readable(const void *addr, size_t size);

// bionic-compatible dl_iterate_phdr over our loaded modules
int so_dl_iterate_phdr(int (*cb)(void *info, size_t size, void *data), void *data);

#endif
