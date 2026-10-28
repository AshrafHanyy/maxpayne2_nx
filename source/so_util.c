/* so_util.c -- multi-module loader for Android AArch64 .so files
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 * Rewritten for multiple modules / RELR / multi-segment ELFs.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>

#include "so_util.h"
#include "util.h"
#include "error.h"

#ifndef DT_RELRSZ
#define DT_RELRSZ 35
#define DT_RELR 36
#endif
#define DT_ANDROID_RELR 0x6fffe000
#define DT_ANDROID_RELRSZ 0x6fffe001
#ifndef DT_GNU_HASH
#define DT_GNU_HASH 0x6ffffef5
#endif
#ifndef R_AARCH64_NONE
#define R_AARCH64_NONE 0
#endif

#define PAGE_SIZE 0x1000

// a crash at this address range means an import we don't provide was called
#define UNRESOLVED_BASE 0xDEAD000000000000ULL

struct so_module {
  so_module *next;
  char name[64];

  uint8_t *load_base;     // staging copy we write relocations/hooks into
  uint8_t *load_virtbase; // code-mapped alias the program runs from
  size_t load_size;
  VirtmemReservation *memrv;
  int finalized;

  Elf64_Phdr *phdr;
  int phnum;

  // all of these are vaddrs relative to the module base
  uintptr_t symtab, strtab, rela, jmprel, relr, init_array, init_func;
  size_t relasz, pltrelsz, relrsz, init_array_sz;
  size_t num_syms;

  // name -> symbol index hash table of defined symbols
  uint32_t *hash;
  size_t hash_mask;

  uintptr_t rx_start, rx_end;
};

static so_module *s_modules = NULL;
static so_module *s_modules_tail = NULL;

static uint8_t *s_region_base = NULL;
static size_t s_region_size = 0;
static size_t s_region_used = 0;

static const DynLibFunction *s_imports = NULL;
static uint32_t *s_import_hash = NULL;
static size_t s_import_mask = 0;

static uint32_t hash_str(const char *s) {
  uint32_t h = 2166136261u;
  while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
  return h;
}

static inline uint8_t *mod_ptr(so_module *mod, uintptr_t vaddr) {
  return (mod->finalized ? mod->load_virtbase : mod->load_base) + vaddr;
}

static inline Elf64_Sym *mod_sym(so_module *mod, size_t i) {
  return (Elf64_Sym *)mod_ptr(mod, mod->symtab) + i;
}

static inline const char *mod_str(so_module *mod, uint32_t off) {
  return (const char *)mod_ptr(mod, mod->strtab) + off;
}

void so_init_region(void *base, size_t size) {
  s_region_base = base;
  s_region_size = size;
  s_region_used = 0;
}

void so_set_imports(const DynLibFunction *funcs, size_t num) {
  size_t cap = 1;
  while (cap < num * 2) cap <<= 1;
  s_imports = funcs;
  s_import_mask = cap - 1;
  s_import_hash = calloc(cap, sizeof(uint32_t));
  for (size_t i = 0; i < num; ++i) {
    size_t h = hash_str(funcs[i].symbol) & s_import_mask;
    while (s_import_hash[h]) {
      if (!strcmp(funcs[s_import_hash[h] - 1].symbol, funcs[i].symbol))
        fatal_error("duplicate import entry: %s", funcs[i].symbol);
      h = (h + 1) & s_import_mask;
    }
    s_import_hash[h] = i + 1;
  }
}

static const DynLibFunction *find_import(const char *name) {
  if (!s_import_hash) return NULL;
  size_t h = hash_str(name) & s_import_mask;
  while (s_import_hash[h]) {
    const DynLibFunction *f = &s_imports[s_import_hash[h] - 1];
    if (!strcmp(f->symbol, name)) return f;
    h = (h + 1) & s_import_mask;
  }
  return NULL;
}

static size_t gnu_hash_count(const uint32_t *h) {
  const uint32_t nbuckets = h[0], symoffset = h[1], bloom_size = h[2];
  const uint32_t *buckets = (const uint32_t *)((const uint64_t *)(h + 4) + bloom_size);
  const uint32_t *chain = buckets + nbuckets;
  uint32_t max = 0;
  for (uint32_t i = 0; i < nbuckets; ++i)
    if (buckets[i] > max) max = buckets[i];
  if (max < symoffset) return symoffset;
  while (!(chain[max - symoffset] & 1)) ++max;
  return max + 1;
}

static void build_symbol_hash(so_module *mod) {
  size_t cap = 1;
  while (cap < mod->num_syms * 2) cap <<= 1;
  mod->hash = calloc(cap, sizeof(uint32_t));
  mod->hash_mask = cap - 1;
  for (size_t i = 1; i < mod->num_syms; ++i) {
    const Elf64_Sym *s = mod_sym(mod, i);
    if (s->st_shndx == SHN_UNDEF || !s->st_name) continue;
    const char *name = mod_str(mod, s->st_name);
    size_t h = hash_str(name) & mod->hash_mask;
    while (mod->hash[h]) h = (h + 1) & mod->hash_mask;
    mod->hash[h] = i;
  }
}

static const Elf64_Sym *find_defined(so_module *mod, const char *name) {
  size_t h = hash_str(name) & mod->hash_mask;
  while (mod->hash[h]) {
    const Elf64_Sym *s = mod_sym(mod, mod->hash[h]);
    if (!strcmp(mod_str(mod, s->st_name), name)) return s;
    h = (h + 1) & mod->hash_mask;
  }
  return NULL;
}

so_module *so_load(const char *filename) {
  FILE *fd = fopen(filename, "rb");
  if (!fd) return NULL;
  fseek(fd, 0, SEEK_END);
  const size_t file_size = ftell(fd);
  fseek(fd, 0, SEEK_SET);
  uint8_t *file = malloc(file_size);
  if (!file || fread(file, file_size, 1, fd) != 1)
    fatal_error("Could not read %s.", filename);
  fclose(fd);

  const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)file;
  if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) || ehdr->e_machine != EM_AARCH64 || ehdr->e_ident[EI_CLASS] != ELFCLASS64)
    fatal_error("%s is not an AArch64 ELF.\nMake sure it comes from lib/arm64-v8a.", filename);

  so_module *mod = calloc(1, sizeof(*mod));
  strncpy(mod->name, filename, sizeof(mod->name) - 1);
  mod->phnum = ehdr->e_phnum;
  mod->phdr = malloc(sizeof(Elf64_Phdr) * mod->phnum);
  memcpy(mod->phdr, file + ehdr->e_phoff, sizeof(Elf64_Phdr) * mod->phnum);

  // size of the loaded image and the executable range
  int num_rx = 0;
  for (int i = 0; i < mod->phnum; ++i) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type != PT_LOAD) continue;
    const size_t end = p->p_vaddr + p->p_memsz;
    if (end > mod->load_size) mod->load_size = end;
    if (p->p_flags & PF_X) {
      mod->rx_start = p->p_vaddr & ~(uintptr_t)(PAGE_SIZE - 1);
      mod->rx_end = ALIGN_MEM(p->p_vaddr + p->p_memsz, PAGE_SIZE);
      ++num_rx;
    }
  }
  if (num_rx != 1)
    fatal_error("%s: expected exactly one executable segment, got %d.", filename, num_rx);
  mod->load_size = ALIGN_MEM(mod->load_size, PAGE_SIZE);

  // check that no writable segment shares a page with the executable one
  for (int i = 0; i < mod->phnum; ++i) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type == PT_LOAD && !(p->p_flags & PF_X) && p->p_vaddr < mod->rx_end && p->p_vaddr + p->p_memsz > mod->rx_start)
      fatal_error("%s: writable segment overlaps code pages.", filename);
  }

  if (s_region_used + mod->load_size > s_region_size)
    fatal_error("Out of module memory loading %s\n(need %zu KB more).", filename, (s_region_used + mod->load_size - s_region_size) / 1024);
  mod->load_base = s_region_base + s_region_used;
  s_region_used += mod->load_size;
  memset(mod->load_base, 0, mod->load_size);

  for (int i = 0; i < mod->phnum; ++i) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type == PT_LOAD)
      memcpy(mod->load_base + p->p_vaddr, file + p->p_offset, p->p_filesz);
  }
  free(file);

  virtmemLock();
  mod->load_virtbase = virtmemFindCodeMemory(mod->load_size, PAGE_SIZE);
  mod->memrv = virtmemAddReservation(mod->load_virtbase, mod->load_size);
  virtmemUnlock();
  if (!mod->load_virtbase || !mod->memrv)
    fatal_error("%s: could not reserve address space.", filename);

  // parse the dynamic section
  const Elf64_Dyn *dyn = NULL;
  for (int i = 0; i < mod->phnum; ++i)
    if (mod->phdr[i].p_type == PT_DYNAMIC)
      dyn = (const Elf64_Dyn *)(mod->load_base + mod->phdr[i].p_vaddr);
  if (!dyn)
    fatal_error("%s: no dynamic section.", filename);

  uintptr_t hash = 0, gnu_hash = 0;
  for (; dyn->d_tag != DT_NULL; ++dyn) {
    switch (dyn->d_tag) {
      case DT_SYMTAB: mod->symtab = dyn->d_un.d_ptr; break;
      case DT_STRTAB: mod->strtab = dyn->d_un.d_ptr; break;
      case DT_RELA: mod->rela = dyn->d_un.d_ptr; break;
      case DT_RELASZ: mod->relasz = dyn->d_un.d_val; break;
      case DT_JMPREL: mod->jmprel = dyn->d_un.d_ptr; break;
      case DT_PLTRELSZ: mod->pltrelsz = dyn->d_un.d_val; break;
      case DT_RELR: case DT_ANDROID_RELR: mod->relr = dyn->d_un.d_ptr; break;
      case DT_RELRSZ: case DT_ANDROID_RELRSZ: mod->relrsz = dyn->d_un.d_val; break;
      case DT_INIT_ARRAY: mod->init_array = dyn->d_un.d_ptr; break;
      case DT_INIT_ARRAYSZ: mod->init_array_sz = dyn->d_un.d_val; break;
      case DT_INIT: mod->init_func = dyn->d_un.d_ptr; break;
      case DT_HASH: hash = dyn->d_un.d_ptr; break;
      case DT_GNU_HASH: gnu_hash = dyn->d_un.d_ptr; break;
      case DT_PLTREL:
        if (dyn->d_un.d_val != DT_RELA) fatal_error("%s: PLT uses REL relocations.", filename);
        break;
      case 0x60000011: case 0x60000012: // DT_ANDROID_REL/RELA (packed)
        fatal_error("%s: Android packed relocations are not supported.", filename);
        break;
      default: break;
    }
  }
  if (!mod->symtab || !mod->strtab)
    fatal_error("%s: missing symbol table.", filename);
  if (hash)
    mod->num_syms = ((const uint32_t *)(mod->load_base + hash))[1];
  else if (gnu_hash)
    mod->num_syms = gnu_hash_count((const uint32_t *)(mod->load_base + gnu_hash));
  else
    fatal_error("%s: no symbol hash table.", filename);

  build_symbol_hash(mod);

  if (s_modules_tail) s_modules_tail->next = mod;
  else s_modules = mod;
  s_modules_tail = mod;

  debugPrintf("loaded %s: %zu KB at %p (staging %p), %zu symbols\n",
    filename, mod->load_size / 1024, mod->load_virtbase, mod->load_base, mod->num_syms);

  return mod;
}

static uintptr_t resolve_symbol(so_module *mod, uint32_t symidx, int *missing) {
  const Elf64_Sym *sym = mod_sym(mod, symidx);
  const char *name = mod_str(mod, sym->st_name);
  *missing = 0;

  if (sym->st_shndx != SHN_UNDEF)
    return (uintptr_t)mod->load_virtbase + sym->st_value;

  const DynLibFunction *imp = find_import(name);
  if (imp) return imp->func;

  for (so_module *m = s_modules; m; m = m->next) {
    if (m == mod) continue;
    const Elf64_Sym *s = find_defined(m, name);
    if (s) return (uintptr_t)m->load_virtbase + s->st_value;
  }

  if (ELF64_ST_BIND(sym->st_info) == STB_WEAK)
    return 0;

  *missing = 1;
  debugPrintf("%s: unresolved import #%u: %s\n", mod->name, symidx, name);
  return UNRESOLVED_BASE | ((uintptr_t)symidx << 4);
}

static void apply_rela(so_module *mod, const Elf64_Rela *rels, size_t count, int *num_missing) {
  for (size_t i = 0; i < count; ++i) {
    const Elf64_Rela *r = &rels[i];
    uintptr_t *ptr = (uintptr_t *)(mod->load_base + r->r_offset);
    const uint32_t type = ELF64_R_TYPE(r->r_info);
    const uint32_t symidx = ELF64_R_SYM(r->r_info);
    int missing = 0;
    switch (type) {
      case R_AARCH64_NONE:
        break;
      case R_AARCH64_RELATIVE:
        *ptr = (uintptr_t)mod->load_virtbase + r->r_addend;
        break;
      case R_AARCH64_ABS64:
      case R_AARCH64_GLOB_DAT:
      case R_AARCH64_JUMP_SLOT: {
        const uintptr_t s = resolve_symbol(mod, symidx, &missing);
        *ptr = s ? s + r->r_addend : 0;
        *num_missing += missing;
        break;
      }
      default:
        fatal_error("%s: unsupported relocation type %u.", mod->name, type);
    }
  }
}

void so_relocate(so_module *mod) {
  int num_missing = 0;

  if (mod->relr) {
    const uint64_t *relr = (const uint64_t *)(mod->load_base + mod->relr);
    const size_t count = mod->relrsz / sizeof(uint64_t);
    uintptr_t *where = NULL;
    for (size_t i = 0; i < count; ++i) {
      const uint64_t e = relr[i];
      if ((e & 1) == 0) {
        where = (uintptr_t *)(mod->load_base + e);
        *where++ += (uintptr_t)mod->load_virtbase;
      } else {
        uint64_t bits = e >> 1;
        for (int b = 0; bits; ++b, bits >>= 1)
          if (bits & 1) where[b] += (uintptr_t)mod->load_virtbase;
        where += 63;
      }
    }
  }

  if (mod->rela)
    apply_rela(mod, (const Elf64_Rela *)(mod->load_base + mod->rela), mod->relasz / sizeof(Elf64_Rela), &num_missing);
  if (mod->jmprel)
    apply_rela(mod, (const Elf64_Rela *)(mod->load_base + mod->jmprel), mod->pltrelsz / sizeof(Elf64_Rela), &num_missing);

  debugPrintf("%s: relocated, %d unresolved imports\n", mod->name, num_missing);
}

void so_finalize(so_module *mod) {
  Result rc;
  const Handle self = envGetOwnProcessHandle();

  armDCacheFlush(mod->load_base, mod->load_size);

  rc = svcMapProcessCodeMemory(self, (u64)mod->load_virtbase, (u64)mod->load_base, mod->load_size);
  if (R_FAILED(rc)) fatal_error("%s: svcMapProcessCodeMemory failed:\n%08x", mod->name, rc);

  if (mod->rx_start > 0) {
    rc = svcSetProcessMemoryPermission(self, (u64)mod->load_virtbase, mod->rx_start, Perm_Rw);
    if (R_FAILED(rc)) fatal_error("%s: could not set RW (pre):\n%08x", mod->name, rc);
  }
  rc = svcSetProcessMemoryPermission(self, (u64)mod->load_virtbase + mod->rx_start, mod->rx_end - mod->rx_start, Perm_Rx);
  if (R_FAILED(rc)) fatal_error("%s: could not set RX:\n%08x", mod->name, rc);
  if (mod->rx_end < mod->load_size) {
    rc = svcSetProcessMemoryPermission(self, (u64)mod->load_virtbase + mod->rx_end, mod->load_size - mod->rx_end, Perm_Rw);
    if (R_FAILED(rc)) fatal_error("%s: could not set RW:\n%08x", mod->name, rc);
  }

  mod->finalized = 1;
  armICacheInvalidate(mod->load_virtbase, mod->load_size);
}

void so_run_init(so_module *mod) {
  if (mod->init_func) {
    debugPrintf("%s: DT_INIT\n", mod->name);
    ((void (*)(void))(mod->load_virtbase + mod->init_func))();
  }
  if (mod->init_array) {
    uintptr_t *arr = (uintptr_t *)(mod->load_virtbase + mod->init_array);
    const size_t count = mod->init_array_sz / sizeof(uintptr_t);
    debugPrintf("%s: running %zu initializers\n", mod->name, count);
    for (size_t i = 0; i < count; ++i)
      if (arr[i] && arr[i] != (uintptr_t)-1)
        ((void (*)(void))arr[i])();
  }
}

static const Elf64_Sym *lookup_or_fail(so_module *mod, const char *name, int fail) {
  const Elf64_Sym *s = find_defined(mod, name);
  if (!s && fail)
    fatal_error("Could not find symbol\n%s\nin %s.\nThis loader needs Max Payne Mobile 2.1.131.", name, mod->name);
  return s;
}

uintptr_t so_symbol(so_module *mod, const char *name) {
  return (uintptr_t)mod->load_virtbase + lookup_or_fail(mod, name, 1)->st_value;
}

uintptr_t so_symbol_opt(so_module *mod, const char *name) {
  const Elf64_Sym *s = lookup_or_fail(mod, name, 0);
  return s ? (uintptr_t)mod->load_virtbase + s->st_value : 0;
}

void so_hook(so_module *mod, const char *name, uintptr_t dst) {
  if (mod->finalized) fatal_error("so_hook(%s) after finalize", name);
  const Elf64_Sym *s = lookup_or_fail(mod, name, 1);
  if (s->st_size < 16) fatal_error("so_hook: %s is too small (%lu bytes)", name, (unsigned long)s->st_size);
  uint32_t *hook = (uint32_t *)(mod->load_base + s->st_value);
  hook[0] = 0x58000051u; // LDR X17, #0x8
  hook[1] = 0xd61f0220u; // BR X17
  *(uint64_t *)(hook + 2) = dst;
}

void so_patch_ret(so_module *mod, const char *name, uint32_t value) {
  if (mod->finalized) fatal_error("so_patch_ret(%s) after finalize", name);
  if (value > 0xFFFF) fatal_error("so_patch_ret: value too large");
  const Elf64_Sym *s = lookup_or_fail(mod, name, 1);
  if (s->st_size < 8) fatal_error("so_patch_ret: %s is too small", name);
  uint32_t *code = (uint32_t *)(mod->load_base + s->st_value);
  code[0] = 0x52800000u | (value << 5); // MOV W0, #value
  code[1] = 0xd65f03c0u;                // RET
}

int so_is_readable(const void *addr, size_t size) {
  MemoryInfo info;
  u32 page;
  if (R_FAILED(svcQueryMemory(&info, &page, (u64)addr))) return 0;
  if (!(info.perm & Perm_R)) return 0;
  return (u64)addr + size <= info.addr + info.size;
}

void so_describe_addr(uintptr_t addr, char *out, size_t outsize) {
  out[0] = 0;
  for (so_module *m = s_modules; m; m = m->next) {
    const uintptr_t base = (uintptr_t)m->load_virtbase;
    if (addr < base || addr >= base + m->load_size) continue;
    const uintptr_t off = addr - base;
    const char *best = NULL;
    uintptr_t best_val = 0;
    if (m->finalized) {
      for (size_t i = 1; i < m->num_syms; ++i) {
        const Elf64_Sym *s = mod_sym(m, i);
        if (s->st_shndx == SHN_UNDEF || ELF64_ST_TYPE(s->st_info) != STT_FUNC) continue;
        if (s->st_value <= off && s->st_value >= best_val) {
          best_val = s->st_value;
          best = mod_str(m, s->st_name);
        }
      }
    }
    if (best)
      snprintf(out, outsize, "%s+0x%lx (%s+0x%lx)", m->name, off, best, off - best_val);
    else
      snprintf(out, outsize, "%s+0x%lx", m->name, off);
    return;
  }
  if ((addr & 0xFFFF000000000000ULL) == UNRESOLVED_BASE) {
    snprintf(out, outsize, "<unresolved import #%lu, see debug.log>", (unsigned long)((addr & 0xFFFFFFFFFFFFULL) >> 4));
    return;
  }
  // our own code: offsets match addresses in maxpayne.elf (addr2line -e maxpayne.elf)
  extern char __start__[];
  const uintptr_t nro = (uintptr_t)__start__;
  if (addr >= nro && addr < nro + 0x4000000)
    snprintf(out, outsize, "maxpayne.nro+0x%lx", (unsigned long)(addr - nro));
}

// matches bionic's struct dl_phdr_info
typedef struct {
  Elf64_Addr dlpi_addr;
  const char *dlpi_name;
  const Elf64_Phdr *dlpi_phdr;
  Elf64_Half dlpi_phnum;
  unsigned long long dlpi_adds;
  unsigned long long dlpi_subs;
  size_t dlpi_tls_modid;
  void *dlpi_tls_data;
} bionic_dl_phdr_info;

int so_dl_iterate_phdr(int (*cb)(void *info, size_t size, void *data), void *data) {
  int ret = 0;
  for (so_module *m = s_modules; m; m = m->next) {
    if (!m->finalized) continue;
    bionic_dl_phdr_info info = {
      .dlpi_addr = (Elf64_Addr)m->load_virtbase,
      .dlpi_name = m->name,
      .dlpi_phdr = m->phdr,
      .dlpi_phnum = m->phnum,
      .dlpi_adds = 1,
      .dlpi_subs = 0,
    };
    ret = cb(&info, sizeof(info), data);
    if (ret) break;
  }
  return ret;
}
