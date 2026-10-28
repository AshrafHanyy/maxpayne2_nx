/* bionic_libc.c -- bionic libc ABI shims on top of newlib
 *
 * Covers struct layout differences (FILE array, stat, dirent, lconv),
 * constant differences (clock ids, open flags, sysconf names) and
 * bionic-only functions (fortify *_chk, __system_property_get, ...).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <wchar.h>
#include <wctype.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>

#include "util.h"
#include "error.h"
#include "so_util.h"

// stdio: bionic's stdin/stdout/stderr are &__sF[0..2], sizeof(FILE) == 152

#define BIONIC_FILE_SIZE 152
uint8_t fake_sF[3 * BIONIC_FILE_SIZE] __attribute__((aligned(16)));

static FILE *F(void *f) {
  const uintptr_t p = (uintptr_t)f, b = (uintptr_t)fake_sF;
  if (p >= b && p < b + sizeof(fake_sF)) {
    switch ((p - b) / BIONIC_FILE_SIZE) {
      case 0: return stdin;
      default: return log_file() ? log_file() : stderr;
    }
  }
  return f;
}

static int is_fake_std(void *f) {
  const uintptr_t p = (uintptr_t)f, b = (uintptr_t)fake_sF;
  return p >= b && p < b + sizeof(fake_sF);
}

int fclose_fake(void *f) { return is_fake_std(f) ? 0 : fclose(F(f)); }
int feof_fake(void *f) { return feof(F(f)); }
int ferror_fake(void *f) { return ferror(F(f)); }
int fflush_fake(void *f) { return fflush(f ? F(f) : NULL); }
int fgetc_fake(void *f) { return fgetc(F(f)); }
int getc_fake(void *f) { return getc(F(f)); }
char *fgets_fake(char *s, int n, void *f) { return fgets(s, n, F(f)); }
int fputc_fake(int c, void *f) { return fputc(c, F(f)); }
int fputs_fake(const char *s, void *f) { return fputs(s, F(f)); }
wint_t fputwc_fake(wchar_t c, void *f) { return fputwc(c, F(f)); }
wint_t getwc_fake(void *f) { return getwc(F(f)); }
wint_t ungetwc_fake(wint_t c, void *f) { return ungetwc(c, F(f)); }
int ungetc_fake(int c, void *f) { return ungetc(c, F(f)); }
size_t fread_fake(void *p, size_t s, size_t n, void *f) { return fread(p, s, n, F(f)); }
size_t fwrite_fake(const void *p, size_t s, size_t n, void *f) { return fwrite(p, s, n, F(f)); }
int fseek_fake(void *f, long o, int w) { return fseek(F(f), o, w); }
int fseeko_fake(void *f, off_t o, int w) { return fseeko(F(f), o, w); }
long ftell_fake(void *f) { return ftell(F(f)); }
off_t ftello_fake(void *f) { return ftello(F(f)); }
int fileno_fake(void *f) { return fileno(F(f)); }
void setbuf_fake(void *f, char *b) { setbuf(F(f), b); }
int vfprintf_fake(void *f, const char *fmt, va_list ap) { return vfprintf(F(f), fmt, ap); }

int fprintf_fake(void *f, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const int r = vfprintf(F(f), fmt, ap);
  va_end(ap);
  return r;
}

int printf_fake(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  debugVPrintf(fmt, ap);
  va_end(ap);
  return 0;
}

// open flags (bionic uses Linux values)

static int conv_oflags(int f) {
  int r = f & 3;
  if (f & 0100) r |= O_CREAT;
  if (f & 0200) r |= O_EXCL;
  if (f & 01000) r |= O_TRUNC;
  if (f & 02000) r |= O_APPEND;
  return r;
}

int open_fake(const char *path, int flags, int mode) {
  return open(path, conv_oflags(flags), mode);
}

// struct stat (bionic aarch64 layout)

typedef struct {
  uint64_t st_dev;
  uint64_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint64_t __pad1;
  int64_t st_size;
  int32_t st_blksize;
  int32_t __pad2;
  int64_t st_blocks;
  int64_t st_atime_sec, st_atime_nsec;
  int64_t st_mtime_sec, st_mtime_nsec;
  int64_t st_ctime_sec, st_ctime_nsec;
  uint32_t __unused4, __unused5;
} bionic_stat;

static void conv_stat(const struct stat *s, bionic_stat *b) {
  memset(b, 0, sizeof(*b));
  b->st_dev = s->st_dev;
  b->st_ino = s->st_ino;
  b->st_mode = s->st_mode;
  b->st_nlink = s->st_nlink;
  b->st_size = s->st_size;
  b->st_blksize = 512;
  b->st_blocks = (s->st_size + 511) / 512;
  b->st_atime_sec = s->st_atime;
  b->st_mtime_sec = s->st_mtime;
  b->st_ctime_sec = s->st_ctime;
}

int stat_fake(const char *path, bionic_stat *out) {
  struct stat s;
  const int r = stat(path, &s);
  if (r == 0) conv_stat(&s, out);
  return r;
}

int fstat_fake(int fd, bionic_stat *out) {
  struct stat s;
  const int r = fstat(fd, &s);
  if (r == 0) conv_stat(&s, out);
  return r;
}

// dirent (bionic dirent and dirent64 are identical on LP64)

typedef struct {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[256];
} bionic_dirent;

typedef struct {
  DIR *dir;
  bionic_dirent ent;
} bionic_DIR;

void *opendir_fake(const char *path) {
  DIR *d = opendir(path);
  if (!d) return NULL;
  bionic_DIR *bd = calloc(1, sizeof(*bd));
  bd->dir = d;
  return bd;
}

void *readdir_fake(void *dir) {
  bionic_DIR *bd = dir;
  struct dirent *e = readdir(bd->dir);
  if (!e) return NULL;
  memset(&bd->ent, 0, sizeof(bd->ent));
  bd->ent.d_ino = e->d_ino;
  bd->ent.d_type = e->d_type;
  bd->ent.d_reclen = sizeof(bd->ent);
  strncpy(bd->ent.d_name, e->d_name, sizeof(bd->ent.d_name) - 1);
  return &bd->ent;
}

int closedir_fake(void *dir) {
  bionic_DIR *bd = dir;
  if (!bd) return -1;
  const int r = closedir(bd->dir);
  free(bd);
  return r;
}

// time (bionic uses Linux clock ids)

int clock_gettime_fake(int clk, struct timespec *ts) {
  return clock_gettime(clk == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC, ts);
}

// fortify

void *__memcpy_chk_fake(void *d, const void *s, size_t n, size_t dl) { return memcpy(d, s, n); }
void *__memmove_chk_fake(void *d, const void *s, size_t n, size_t dl) { return memmove(d, s, n); }
char *__strcat_chk_fake(char *d, const char *s, size_t dl) { return strcat(d, s); }
char *__strchr_chk_fake(const char *s, int c, size_t sl) { return strchr(s, c); }
char *__strcpy_chk_fake(char *d, const char *s, size_t dl) { return strcpy(d, s); }
size_t __strlen_chk_fake(const char *s, size_t sl) { return strlen(s); }
char *__strncat_chk_fake(char *d, const char *s, size_t n, size_t dl) { return strncat(d, s, n); }
char *__strncpy_chk_fake(char *d, const char *s, size_t n, size_t dl) { return strncpy(d, s, n); }
char *__strncpy_chk2_fake(char *d, const char *s, size_t n, size_t dl, size_t sl) { return strncpy(d, s, n); }
int __vsnprintf_chk_fake(char *b, size_t n, int fl, size_t bl, const char *fmt, va_list ap) { return vsnprintf(b, n, fmt, ap); }
int __vsprintf_chk_fake(char *b, int fl, size_t bl, const char *fmt, va_list ap) { return vsprintf(b, fmt, ap); }

void __stack_chk_fail_fake(void) {
  fatal_error("Stack corruption detected (__stack_chk_fail).");
}

void abort_fake(void) {
  fatal_error("The game called abort().\nCheck debug.log for the last messages.");
}

void android_set_abort_message_fake(const char *msg) {
  debugPrintf("abort message: %s\n", msg ? msg : "(null)");
}

// misc bionic-only

int __system_property_get_fake(const char *name, char *value) {
  value[0] = 0;
  return 0;
}

unsigned long getauxval_fake(unsigned long type) { return 0; }

int __register_atfork_fake(void *a, void *b, void *c, void *d) { return 0; }

int __cxa_atexit_fake(void (*fn)(void *), void *arg, void *dso) { return 0; }
void __cxa_finalize_fake(void *dso) { }

int gettid_fake(void) {
  u64 id = 0;
  svcGetThreadId(&id, CUR_THREAD_HANDLE);
  return (int)id;
}

// arm64 Linux syscall numbers
#define SYS_futex 98
#define SYS_gettid 178
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

long syscall_fake(long n, long a1, long a2, long a3, long a4, long a5) {
  if (n == SYS_gettid)
    return gettid_fake();
  if (n == SYS_futex) {
    const int op = a2 & 0x7f;
    if (op == FUTEX_WAIT) {
      if (__atomic_load_n((int *)a1, __ATOMIC_ACQUIRE) != (int)a3) {
        errno = EAGAIN;
        return -1;
      }
      svcSleepThread(50000);
      return 0;
    }
    if (op == FUTEX_WAKE) return 0;
  }
  debugPrintf("syscall(%ld) not supported\n", n);
  errno = ENOSYS;
  return -1;
}

// bionic _SC_* values
long sysconf_fake(int name) {
  switch (name) {
    case 0x27: case 0x28: return 0x1000;  // _SC_PAGESIZE, _SC_PAGE_SIZE
    case 0x60: case 0x61: return 3;       // _SC_NPROCESSORS_CONF/ONLN
    default: return -1;
  }
}

int dl_iterate_phdr_fake(int (*cb)(void *, size_t, void *), void *data) {
  return so_dl_iterate_phdr(cb, data);
}

void sincosf_fake(float x, float *s, float *c) {
  *s = sinf(x);
  *c = cosf(x);
}

int strerror_r_fake(int e, char *buf, size_t n) {
  if (!n) return EINVAL;
  strncpy(buf, strerror(e), n - 1);
  buf[n - 1] = 0;
  return 0;
}

int posix_memalign_fake(void **out, size_t align, size_t size) {
  if (align < sizeof(void *) || (align & (align - 1))) return EINVAL;
  void *p = aligned_alloc(align, (size + align - 1) & ~(align - 1));
  if (!p) return ENOMEM;
  *out = p;
  return 0;
}

int enosys_fake(void) {
  errno = ENOSYS;
  return -1;
}

void *null_fake(void) {
  errno = ENOSYS;
  return NULL;
}

void nop_fake(void) { }

int isatty_fake(int fd) { return 0; }

// locale: everything runs in the "C" locale

static int s_dummy_locale;

void *newlocale_fake(int mask, const char *name, void *base) { return &s_dummy_locale; }
void *uselocale_fake(void *loc) { return &s_dummy_locale; }
void freelocale_fake(void *loc) { }
char *setlocale_fake(int cat, const char *loc) { return "C"; }
size_t __ctype_get_mb_cur_max_fake(void) { return 1; }

// bionic struct lconv (standard member order)
static struct {
  char *decimal_point, *thousands_sep, *grouping, *int_curr_symbol, *currency_symbol;
  char *mon_decimal_point, *mon_thousands_sep, *mon_grouping, *positive_sign, *negative_sign;
  char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes;
  char n_sep_by_space, p_sign_posn, n_sign_posn, int_p_cs_precedes, int_p_sep_by_space;
  char int_n_cs_precedes, int_n_sep_by_space, int_p_sign_posn, int_n_sign_posn;
} s_lconv = {
  ".", "", "", "", "", "", "", "", "", "",
  CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX,
  CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX,
};

void *localeconv_fake(void) { return &s_lconv; }

int iswalpha_l_fake(wint_t c, void *l) { return iswalpha(c); }
int iswblank_l_fake(wint_t c, void *l) { return iswblank(c); }
int iswcntrl_l_fake(wint_t c, void *l) { return iswcntrl(c); }
int iswdigit_l_fake(wint_t c, void *l) { return iswdigit(c); }
int iswlower_l_fake(wint_t c, void *l) { return iswlower(c); }
int iswprint_l_fake(wint_t c, void *l) { return iswprint(c); }
int iswpunct_l_fake(wint_t c, void *l) { return iswpunct(c); }
int iswspace_l_fake(wint_t c, void *l) { return iswspace(c); }
int iswupper_l_fake(wint_t c, void *l) { return iswupper(c); }
int iswxdigit_l_fake(wint_t c, void *l) { return iswxdigit(c); }
wint_t towlower_l_fake(wint_t c, void *l) { return towlower(c); }
wint_t towupper_l_fake(wint_t c, void *l) { return towupper(c); }
int strcoll_l_fake(const char *a, const char *b, void *l) { return strcoll(a, b); }
size_t strxfrm_l_fake(char *d, const char *s, size_t n, void *l) { return strxfrm(d, s, n); }
int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *l) { return wcscoll(a, b); }
size_t wcsxfrm_l_fake(wchar_t *d, const wchar_t *s, size_t n, void *l) { return wcsxfrm(d, s, n); }
size_t strftime_l_fake(char *s, size_t n, const char *fmt, const struct tm *t, void *l) { return strftime(s, n, fmt, t); }
long double strtold_l_fake(const char *s, char **e, void *l) { return strtold(s, e); }
long long strtoll_l_fake(const char *s, char **e, int b, void *l) { return strtoll(s, e, b); }
unsigned long long strtoull_l_fake(const char *s, char **e, int b, void *l) { return strtoull(s, e, b); }

// syslog: just log
void syslog_fake(int prio, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  debugVPrintf(fmt, ap);
  va_end(ap);
  debugPrintf("\n");
}
