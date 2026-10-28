/* imports.c -- symbols provided to libGame.so and libc++_shared.so
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 * Reworked for Max Payne Mobile 2.x (bionic ABI shims, libc++_shared).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <setjmp.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <mpg123.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "imports.h"
#include "gl_trace.h"

extern uint8_t fake_sF[];

// shims implemented in bionic_*.c / android_ndk.c (all named <symbol>_fake)
#define FAKE_LIST(X) \
  X(AAssetManager_fromJava) X(AAssetManager_open) X(AAsset_close) X(AAsset_getLength64) \
  X(AAsset_getRemainingLength64) X(AAsset_read) X(AAsset_seek64) \
  X(ANativeWindow_fromSurface) X(ANativeWindow_getHeight) X(ANativeWindow_getWidth) \
  X(ANativeWindow_release) X(ANativeWindow_setBuffersGeometry) X(__android_log_print) \
  X(__cxa_atexit) X(__cxa_finalize) X(__memcpy_chk) X(__memmove_chk) X(__register_atfork) \
  X(__stack_chk_fail) X(__strcat_chk) X(__strchr_chk) X(__strcpy_chk) X(__strlen_chk) \
  X(__strncat_chk) X(__strncpy_chk) X(__strncpy_chk2) X(__system_property_get) \
  X(__vsnprintf_chk) X(__vsprintf_chk) X(abort) X(android_set_abort_message) \
  X(closedir) X(opendir) X(dl_iterate_phdr) X(getauxval) X(gettid) X(syscall) X(sysconf) \
  X(fclose) X(feof) X(ferror) X(fflush) X(fgetc) X(fgets) X(fprintf) X(fread) X(fseek) \
  X(fseeko) X(ftell) X(ftello) X(fwrite) X(fileno) X(fputc) X(fputs) X(fputwc) X(getc) X(getwc) \
  X(ungetc) X(ungetwc) X(vfprintf) X(setbuf) X(printf) X(open) X(isatty) \
  X(stat) X(fstat) X(clock_gettime) X(sincosf) X(strerror_r) X(syslog) X(posix_memalign) \
  X(pthread_attr_init) X(pthread_attr_destroy) X(pthread_attr_getstacksize) \
  X(pthread_attr_getschedparam) X(pthread_attr_setschedparam) \
  X(pthread_create) X(pthread_join) X(pthread_detach) X(pthread_self) \
  X(pthread_key_create) X(pthread_key_delete) X(pthread_getspecific) X(pthread_setspecific) \
  X(pthread_mutexattr_init) X(pthread_mutexattr_settype) X(pthread_mutexattr_destroy) \
  X(pthread_mutex_init) X(pthread_mutex_destroy) X(pthread_mutex_lock) X(pthread_mutex_trylock) \
  X(pthread_mutex_unlock) X(pthread_cond_init) X(pthread_cond_destroy) X(pthread_cond_broadcast) \
  X(pthread_cond_signal) X(pthread_cond_wait) X(pthread_cond_timedwait) \
  X(pthread_rwlock_rdlock) X(pthread_rwlock_wrlock) X(pthread_rwlock_unlock) X(pthread_once) \
  X(sem_init) X(sem_destroy) X(sem_wait) X(sem_trywait) X(sem_post) X(sem_getvalue) \
  X(__emutls_get_address) \
  X(newlocale) X(uselocale) X(freelocale) X(setlocale) X(localeconv) X(__ctype_get_mb_cur_max) \
  X(iswalpha_l) X(iswblank_l) X(iswcntrl_l) X(iswdigit_l) X(iswlower_l) X(iswprint_l) \
  X(iswpunct_l) X(iswspace_l) X(iswupper_l) X(iswxdigit_l) X(towlower_l) X(towupper_l) \
  X(strcoll_l) X(strxfrm_l) X(wcscoll_l) X(wcsxfrm_l) X(strftime_l) X(strtold_l) \
  X(strtoll_l) X(strtoull_l)

#define DECL_FAKE(n) extern void n##_fake(void);
FAKE_LIST(DECL_FAKE)

// gl_uniforms.c
extern void glUniform1f_fix(void);
extern void glUniform1fv_fix(void);
extern void glUniform1i_fix(void);
extern void glUniform2fv_fix(void);
extern void glUniform3f_fix(void);
extern void glUniform3fv_fix(void);
extern void glUniform4fv_fix(void);
extern void glUniformMatrix4fv_fix(void);

// gl_debug.c
extern void glCompileShader_dbg(void);
extern void glLinkProgram_dbg(void);
extern void glCheckFramebufferStatus_dbg(void);
extern void glRenderbufferStorage_dbg(void);
extern void glFramebufferTexture2D_dbg(void);
extern void glGetString_dbg(void);
extern void glDrawArrays_dbg(void);
extern void glDrawElements_dbg(void);

extern void enosys_fake(void);
extern void null_fake(void);
extern void nop_fake(void);
extern void readdir_fake(void);

// GL hooks (from max_nx), enabled by config

static void glCompressedTexImage2DHook(GLenum target, GLint level, GLenum format, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data) {
  // don't upload mips
  if (level == 0)
    glCompressedTexImage2D(target, level, format, width, height, border, imageSize, data);
}

static void glTexParameteriHook(GLenum target, GLenum param, GLint val) {
  // force trilinear filtering instead of bilinear+nearest mipmap
  if (val == GL_LINEAR_MIPMAP_NEAREST)
    val = GL_LINEAR_MIPMAP_LINEAR;
  glTexParameteri(target, param, val);
  if (config.debug_log)
    gl_trace_check("glTexParameteri", -1, 0, "0x%x, 0x%x, 0x%x", target, param, val);
}

static void glGetShaderInfoLogHook(GLuint shader, GLsizei maxLength, GLsizei *length, GLchar *infoLog) {
  glGetShaderInfoLog(shader, maxLength, length, infoLog);
  if (infoLog && infoLog[0]) debugPrintf("shader info log:\n%s\n", infoLog);
}

#define FAKE(n) { #n, (uintptr_t)&n##_fake },
#define REAL(n) { #n, (uintptr_t)&n },

static DynLibFunction dynlib_functions[] = {
  FAKE_LIST(FAKE)

  { "__sF", (uintptr_t)fake_sF },
  { "__errno", (uintptr_t)&__errno },
  { "lstat", (uintptr_t)&stat_fake },
  { "readdir", (uintptr_t)&readdir_fake },
  { "readdir64", (uintptr_t)&readdir_fake },
  { "openat", (uintptr_t)&enosys_fake },

  // nothing below is used by the game (std::filesystem etc. in libc++)
  { "fchmod", (uintptr_t)&enosys_fake },
  { "fchmodat", (uintptr_t)&enosys_fake },
  { "fdopendir", (uintptr_t)&null_fake },
  { "ftruncate", (uintptr_t)&enosys_fake },
  { "ioctl", (uintptr_t)&enosys_fake },
  { "link", (uintptr_t)&enosys_fake },
  { "pathconf", (uintptr_t)&enosys_fake },
  { "readlink", (uintptr_t)&enosys_fake },
  { "realpath", (uintptr_t)&null_fake },
  { "sendfile", (uintptr_t)&enosys_fake },
  { "statvfs", (uintptr_t)&enosys_fake },
  { "symlink", (uintptr_t)&enosys_fake },
  { "truncate", (uintptr_t)&enosys_fake },
  { "unlinkat", (uintptr_t)&enosys_fake },
  { "utimensat", (uintptr_t)&enosys_fake },
  { "openlog", (uintptr_t)&nop_fake },
  { "closelog", (uintptr_t)&nop_fake },

  { "pthread_setname_np", (uintptr_t)&ret0 },
  { "pthread_setschedparam", (uintptr_t)&ret0 },
  { "sched_get_priority_min", (uintptr_t)&ret0 },
  { "sched_get_priority_max", (uintptr_t)&ret0 },

  // libc / libm
  REAL(acosf) REAL(asinf) REAL(atan2f) REAL(atanf) REAL(cosf) REAL(sinf) REAL(tanf) REAL(powf)
  REAL(fmod) REAL(fmodf)
  REAL(atoi) REAL(calloc) REAL(free) REAL(malloc) REAL(realloc)
  REAL(exit) REAL(getenv) REAL(qsort) REAL(rand) REAL(srand)
  REAL(gettimeofday) REAL(time) REAL(localtime) REAL(localtime_r) REAL(asctime) REAL(strftime)
  REAL(nanosleep) REAL(usleep)
  REAL(setjmp) REAL(longjmp)
  REAL(memchr) REAL(memcmp) REAL(memcpy) REAL(memmove) REAL(memset)
  REAL(strcasecmp) REAL(strncasecmp) REAL(strcat) REAL(strchr) REAL(strcmp) REAL(strcpy)
  REAL(strdup) REAL(strerror) REAL(strlen) REAL(strncmp) REAL(strncpy) REAL(strstr) REAL(strtok)
  REAL(strtod) REAL(strtof) REAL(strtol) REAL(strtold) REAL(strtoll) REAL(strtoul) REAL(strtoull)
  REAL(snprintf) REAL(sscanf) REAL(vsnprintf) REAL(vasprintf) REAL(vsscanf) REAL(swprintf)
  REAL(fopen) REAL(fdopen) REAL(mkdir) REAL(remove) REAL(rename) REAL(unlink)
  REAL(close) REAL(read) REAL(chdir) REAL(getcwd)
  REAL(btowc) REAL(wctob) REAL(mbrlen) REAL(mbrtowc) REAL(mbsnrtowcs) REAL(mbsrtowcs) REAL(mbtowc)
  REAL(wcrtomb) REAL(wcslen) REAL(wcsnrtombs) REAL(wmemchr) REAL(wmemcmp)
  REAL(wcstod) REAL(wcstof) REAL(wcstol) REAL(wcstold) REAL(wcstoll) REAL(wcstoul) REAL(wcstoull)

  // EGL (Mesa)
  REAL(eglChooseConfig) REAL(eglCreateContext) REAL(eglCreateWindowSurface) REAL(eglDestroySurface)
  REAL(eglGetConfigAttrib) REAL(eglGetDisplay) REAL(eglGetError) REAL(eglInitialize)
  REAL(eglMakeCurrent) REAL(eglSwapBuffers) REAL(eglSwapInterval)

  // GLES 2.0 (Mesa)
  REAL(glActiveTexture) REAL(glAttachShader) REAL(glBindAttribLocation) REAL(glBindBuffer)
  REAL(glBindFramebuffer) REAL(glBindRenderbuffer) REAL(glBindTexture) REAL(glBlendFunc)
  REAL(glBufferData) REAL(glClear) REAL(glClearColor) REAL(glClearDepthf) REAL(glClearStencil)
  REAL(glColorMask) REAL(glCompileShader) REAL(glCompressedTexImage2D) REAL(glCreateProgram)
  REAL(glCreateShader) REAL(glCullFace) REAL(glDeleteBuffers) REAL(glDeleteFramebuffers)
  REAL(glDeleteProgram) REAL(glDeleteRenderbuffers) REAL(glDeleteShader) REAL(glDeleteTextures)
  REAL(glDepthFunc) REAL(glDepthMask) REAL(glDepthRangef) REAL(glDisable)
  REAL(glDisableVertexAttribArray) REAL(glDrawArrays) REAL(glDrawElements) REAL(glEnable)
  REAL(glEnableVertexAttribArray) REAL(glFinish) REAL(glFramebufferRenderbuffer)
  REAL(glFramebufferTexture2D) REAL(glFrontFace) REAL(glGenBuffers) REAL(glGenFramebuffers)
  REAL(glGenRenderbuffers) REAL(glGenTextures) REAL(glGetAttribLocation) REAL(glGetBooleanv)
  REAL(glGetError) REAL(glGetIntegerv) REAL(glGetProgramiv) REAL(glGetShaderiv)
  REAL(glGetString) REAL(glGetUniformLocation) REAL(glIsEnabled) REAL(glIsTexture) REAL(glLinkProgram)
  REAL(glReadPixels) REAL(glRenderbufferStorage) REAL(glScissor) REAL(glShaderSource)
  REAL(glTexImage2D) REAL(glTexParameterf) REAL(glTexParameteri) REAL(glUniform1f) REAL(glUniform1fv)
  REAL(glUniform1i) REAL(glUniform2fv) REAL(glUniform3f) REAL(glUniform3fv) REAL(glUniform4fv)
  REAL(glUniformMatrix4fv) REAL(glUseProgram) REAL(glVertexAttribPointer) REAL(glViewport)
  { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLogHook },

  // OpenAL (openal-soft)
  REAL(alBufferData) REAL(alDeleteBuffers) REAL(alDistanceModel) REAL(alGenBuffers) REAL(alGenSources)
  REAL(alGetEnumValue) REAL(alGetSourcef) REAL(alGetSourcei) REAL(alListener3f) REAL(alListenerf)
  REAL(alListenerfv) REAL(alSource3f) REAL(alSourcePause) REAL(alSourcePlay) REAL(alSourceQueueBuffers)
  REAL(alSourceStop) REAL(alSourceUnqueueBuffers) REAL(alSourcef) REAL(alSourcei)
  REAL(alcCloseDevice) REAL(alcCreateContext) REAL(alcDestroyContext) REAL(alcMakeContextCurrent)
  REAL(alcOpenDevice)

  // mpg123
  REAL(mpg123_delete) REAL(mpg123_exit) REAL(mpg123_feed) REAL(mpg123_feedseek) REAL(mpg123_format_all)
  REAL(mpg123_getformat) REAL(mpg123_info) REAL(mpg123_init) REAL(mpg123_new) REAL(mpg123_open_feed)
  REAL(mpg123_outblock) REAL(mpg123_read)
};

static DynLibFunction *find_entry(const char *name) {
  for (size_t i = 0; i < sizeof(dynlib_functions) / sizeof(*dynlib_functions); ++i)
    if (!strcmp(dynlib_functions[i].symbol, name)) return &dynlib_functions[i];
  return NULL;
}

void imports_init(void) {
  // trace every GL call for errors (slow, gl_trace only); specific wrappers below take precedence
  if (config.gl_trace) {
    for (const gl_trace_entry *e = gl_trace_table; e->name; ++e) {
      DynLibFunction *f = find_entry(e->name);
      if (f) f->func = (uintptr_t)e->func;
    }
  }

  // only use the hooks if the relevant config options are enabled to avoid possible overhead
  if (config.disable_mipmaps)
    find_entry("glCompressedTexImage2D")->func = (uintptr_t)glCompressedTexImage2DHook;
  if (config.trilinear_filter)
    find_entry("glTexParameteri")->func = (uintptr_t)glTexParameteriHook;

  // lenient uniform setters (Mesa rejects type mismatches mobile drivers accept)
  find_entry("glUniform1f")->func = (uintptr_t)glUniform1f_fix;
  find_entry("glUniform1fv")->func = (uintptr_t)glUniform1fv_fix;
  find_entry("glUniform1i")->func = (uintptr_t)glUniform1i_fix;
  find_entry("glUniform2fv")->func = (uintptr_t)glUniform2fv_fix;
  find_entry("glUniform3f")->func = (uintptr_t)glUniform3f_fix;
  find_entry("glUniform3fv")->func = (uintptr_t)glUniform3fv_fix;
  find_entry("glUniform4fv")->func = (uintptr_t)glUniform4fv_fix;
  find_entry("glUniformMatrix4fv")->func = (uintptr_t)glUniformMatrix4fv_fix;

  // shader fixups are always on; the rest only logs
  find_entry("glCompileShader")->func = (uintptr_t)glCompileShader_dbg;
  find_entry("glLinkProgram")->func = (uintptr_t)glLinkProgram_dbg;
  if (config.debug_log) {
    find_entry("glRenderbufferStorage")->func = (uintptr_t)glRenderbufferStorage_dbg;
    find_entry("glFramebufferTexture2D")->func = (uintptr_t)glFramebufferTexture2D_dbg;
    find_entry("glGetString")->func = (uintptr_t)glGetString_dbg;
    find_entry("glDrawArrays")->func = (uintptr_t)glDrawArrays_dbg;
    find_entry("glDrawElements")->func = (uintptr_t)glDrawElements_dbg;
  }
  so_set_imports(dynlib_functions, sizeof(dynlib_functions) / sizeof(*dynlib_functions));
}
