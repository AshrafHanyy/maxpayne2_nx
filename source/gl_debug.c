/* gl_debug.c -- GL call wrappers: shader fixups and rendering diagnostics
 *
 * Mobile GLSL compilers accept things Mesa rejects (most commonly fragment
 * shaders without a default float precision). When a shader fails to
 * compile we patch the source and retry, and log whatever still fails.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <GLES2/gl2.h>

#include "util.h"
#include "gl_trace.h"

#define MAX_TRACE_PER_FUNC 4
#define MAX_TRACE_TOTAL 200

static struct {
  const char *func;
  int count;
} s_trace_counts[96];
static int s_trace_total;

static int trace_should_log(const char *func) {
  if (s_trace_total >= MAX_TRACE_TOTAL) return 0;
  for (size_t i = 0; i < sizeof(s_trace_counts) / sizeof(*s_trace_counts); ++i) {
    if (s_trace_counts[i].func == func || !s_trace_counts[i].func) {
      s_trace_counts[i].func = func;
      if (s_trace_counts[i].count++ >= MAX_TRACE_PER_FUNC) return 0;
      ++s_trace_total;
      return 1;
    }
  }
  return 0;
}

static void log_uniform_name(GLint location) {
  GLint prog = 0, n = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
  if (!prog) {
    debugPrintf(" [no program bound]");
    return;
  }
  glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &n);
  for (GLint i = 0; i < n; ++i) {
    char name[128];
    GLint size = 0;
    GLenum type = 0;
    glGetActiveUniform(prog, i, sizeof(name), NULL, &size, &type, name);
    if (glGetUniformLocation(prog, name) == location) {
      debugPrintf(" [program %d uniform '%s' type 0x%x size %d]", prog, name, type, size);
      return;
    }
  }
  debugPrintf(" [program %d: no uniform at location %d]", prog, location);
}

// one-frame full call trace
#define MAX_FRAME_TRACE_LINES 4000
static int s_frame_trace;
static int s_frame_trace_lines;
static u64 s_trace_deadline;

void gl_trace_arm(float seconds) {
  s_trace_deadline = armGetSystemTick() + (u64)(seconds * armGetSystemTickFreq());
}

int gl_frame_tracing(void) {
  return s_frame_trace && s_frame_trace_lines < MAX_FRAME_TRACE_LINES;
}

void gl_trace_check(const char *func, int location, int is_uniform, const char *fmt, ...) {
  const GLenum err = glGetError();
  if (gl_frame_tracing()) {
    char args[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(args, sizeof(args), fmt, ap);
    va_end(ap);
    ++s_frame_trace_lines;
    debugPrintf("  %s(%s)%s", func, args, err ? " ERROR" : "");
    if (is_uniform && location >= 0) log_uniform_name(location);
    debugPrintf("\n");
  }
  if (err == GL_NO_ERROR) return;
  while (glGetError() != GL_NO_ERROR) { }
  if (!trace_should_log(func)) return;

  char args[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(args, sizeof(args), fmt, ap);
  va_end(ap);
  debugPrintf("GL ERROR 0x%04x in %s(%s)", err, func, args);
  if (is_uniform && location >= 0) log_uniform_name(location);
  debugPrintf("\n");
}

#define MAX_LOGGED_ERRORS 64
#define MAX_LOGGED_SHADERS 16

static int s_logged_errors;
static int s_logged_shaders;
static int s_fixed_shaders;
static unsigned s_draws, s_frame_draws_max;

static void log_shader_source(const char *src) {
  // log in chunks so long shaders aren't cut by the log line buffer
  const size_t len = strlen(src);
  for (size_t i = 0; i < len; i += 1024)
    debugPrintf("%.1024s", src + i);
  debugPrintf("\n--- end of source ---\n");
}

static char *get_source(GLuint shader) {
  GLint len = 0;
  glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &len);
  if (len <= 0) return NULL;
  char *src = malloc(len + 1);
  glGetShaderSource(shader, len + 1, NULL, src);
  return src;
}

static int compiled(GLuint shader) {
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  return ok;
}

// insert a default float precision after the #version line (or at the top)
static char *add_precision(const char *src) {
  static const char line[] = "precision mediump float;\n";
  const size_t len = strlen(src);
  size_t at = 0;
  const char *ver = strstr(src, "#version");
  if (ver) {
    const char *nl = strchr(ver, '\n');
    at = nl ? (size_t)(nl - src) + 1 : len;
  }
  char *out = malloc(len + sizeof(line) + 2);
  memcpy(out, src, at);
  size_t o = at;
  if (o && out[o - 1] != '\n') out[o++] = '\n';
  memcpy(out + o, line, sizeof(line) - 1);
  o += sizeof(line) - 1;
  strcpy(out + o, src + at);
  return out;
}

void glCompileShader_dbg(GLuint shader) {
  glCompileShader(shader);
  if (compiled(shader)) return;

  char log[2048] = { 0 };
  glGetShaderInfoLog(shader, sizeof(log), NULL, log);
  char *src = get_source(shader);
  if (!src) return;

  GLint type = 0;
  glGetShaderiv(shader, GL_SHADER_TYPE, &type);

  if (type == GL_FRAGMENT_SHADER && !strstr(src, "precision ")) {
    char *fixed = add_precision(src);
    const char *srcs[1] = { fixed };
    glShaderSource(shader, 1, srcs, NULL);
    glCompileShader(shader);
    if (compiled(shader)) {
      if (++s_fixed_shaders <= 4)
        debugPrintf("shader %u: fixed by adding default precision\n", shader);
      free(fixed);
      free(src);
      return;
    }
    glGetShaderInfoLog(shader, sizeof(log), NULL, log);
    free(fixed);
  }

  if (s_logged_shaders++ < MAX_LOGGED_SHADERS) {
    debugPrintf("SHADER COMPILE FAILED (%s shader %u):\n%s\n--- source ---\n",
      type == GL_FRAGMENT_SHADER ? "fragment" : "vertex", shader, log);
    log_shader_source(src);
  }
  free(src);
}

void glLinkProgram_dbg(GLuint program) {
  glLinkProgram(program);
  GLint ok = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok && s_logged_shaders++ < MAX_LOGGED_SHADERS) {
    char log[2048] = { 0 };
    glGetProgramInfoLog(program, sizeof(log), NULL, log);
    debugPrintf("PROGRAM LINK FAILED (%u): %s\n", program, log);
    // dump the final sources (after the game's PREFIX/ALPHAKILL substitution)
    GLuint shaders[4];
    GLsizei count = 0;
    glGetAttachedShaders(program, 4, &count, shaders);
    for (GLsizei i = 0; i < count; ++i) {
      char *src = get_source(shaders[i]);
      if (!src) continue;
      debugPrintf("--- attached shader %u ---\n", shaders[i]);
      log_shader_source(src);
      free(src);
    }
  }
}

GLenum glCheckFramebufferStatus_dbg(GLenum target) {
  const GLenum st = glCheckFramebufferStatus(target);
  if (st != GL_FRAMEBUFFER_COMPLETE && s_logged_errors++ < MAX_LOGGED_ERRORS)
    debugPrintf("FRAMEBUFFER INCOMPLETE: 0x%04x\n", st);
  return st;
}

void glRenderbufferStorage_dbg(GLenum target, GLenum fmt, GLsizei w, GLsizei h) {
  debugPrintf("glRenderbufferStorage fmt=0x%04x %dx%d\n", fmt, w, h);
  glRenderbufferStorage(target, fmt, w, h);
}

void glFramebufferTexture2D_dbg(GLenum target, GLenum att, GLenum textarget, GLuint tex, GLint level) {
  debugPrintf("glFramebufferTexture2D att=0x%04x tex=%u\n", att, tex);
  glFramebufferTexture2D(target, att, textarget, tex, level);
}

const GLubyte *glGetString_dbg(GLenum name) {
  const GLubyte *s = glGetString(name);
  static int logged[4];
  const int idx = (name == GL_VENDOR) ? 0 : (name == GL_RENDERER) ? 1 : (name == GL_VERSION) ? 2 : 3;
  if (!logged[idx]) {
    logged[idx] = 1;
    debugPrintf("glGetString(0x%04x) = %s\n", name, s ? (const char *)s : "(null)");
  }
  return s;
}

void glDrawArrays_dbg(GLenum mode, GLint first, GLsizei count) {
  ++s_draws;
  glDrawArrays(mode, first, count);
  gl_trace_check("glDrawArrays", -1, 0, "0x%x, %d, %d", mode, first, count);
}

void glDrawElements_dbg(GLenum mode, GLsizei count, GLenum type, const void *idx) {
  ++s_draws;
  glDrawElements(mode, count, type, idx);
  gl_trace_check("glDrawElements", -1, 0, "0x%x, %d, 0x%x, %p", mode, count, type, idx);
}

// called after every frame on the render thread
void gl_debug_frame(unsigned long frame) {
  GLenum err;
  while ((err = glGetError()) != GL_NO_ERROR) {
    if (s_logged_errors++ < MAX_LOGGED_ERRORS)
      debugPrintf("GL error 0x%04x after frame %lu\n", err, frame);
  }
  if (s_frame_trace) {
    debugPrintf("=== end of frame trace (%d calls) ===\n", s_frame_trace_lines);
    s_frame_trace = 0;
  } else if (s_trace_deadline && armGetSystemTick() >= s_trace_deadline) {
    s_trace_deadline = 0;
    s_frame_trace = 1;
    debugPrintf("=== GL trace of frame %lu ===\n", frame + 1);
  }
  if (s_draws > s_frame_draws_max) s_frame_draws_max = s_draws;
  if (frame % 300 == 0)
    debugPrintf("frame %lu: %u draw calls (max %u)\n", frame, s_draws, s_frame_draws_max);
  s_draws = 0;
}
