/* gl_uniforms.c -- lenient glUniform* calls
 *
 * Mobile GL drivers accept uniform calls whose type doesn't match the
 * uniform's declaration (e.g. glUniform4fv on a vec3, glUniform1f on a
 * sampler). Mesa rejects them with GL_INVALID_OPERATION and leaves the
 * uniform unset. When that happens we look up the uniform's real type and
 * re-issue the values with the matching call.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <string.h>
#include <GLES2/gl2.h>

#include "config.h"
#include "util.h"
#include "gl_trace.h"

#define CACHE_SIZE 1024
#define MAX_FIX_LOGS 24

typedef struct {
  GLint prog, loc;
  GLenum type;
  GLint base; // location of element 0 for arrays
} uniform_info;

static uniform_info s_cache[CACHE_SIZE];
static int s_fix_logs;

static int lookup(GLint loc, GLenum *type, GLint *index) {
  GLint prog = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
  if (!prog || loc < 0) return 0;

  uniform_info *c = &s_cache[((unsigned)prog * 131u + (unsigned)loc) % CACHE_SIZE];
  if (c->prog == prog && c->loc == loc) {
    *type = c->type;
    *index = loc - c->base;
    return 1;
  }

  GLint n = 0;
  glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &n);
  for (GLint i = 0; i < n; ++i) {
    char name[128];
    GLint size = 0;
    GLenum t = 0;
    glGetActiveUniform(prog, i, sizeof(name), NULL, &size, &t, name);
    char *br = strchr(name, '[');
    if (br) *br = 0; // "Bones[0]" -> "Bones"
    const GLint base = glGetUniformLocation(prog, name);
    if (base >= 0 && loc >= base && loc < base + size) {
      *c = (uniform_info){ prog, loc, t, base };
      *type = t;
      *index = loc - base;
      return 1;
    }
  }
  return 0;
}

static int components(GLenum type) {
  switch (type) {
    case GL_FLOAT_VEC2: case GL_INT_VEC2: case GL_BOOL_VEC2: return 2;
    case GL_FLOAT_VEC3: case GL_INT_VEC3: case GL_BOOL_VEC3: return 3;
    case GL_FLOAT_VEC4: case GL_INT_VEC4: case GL_BOOL_VEC4: case GL_FLOAT_MAT2: return 4;
    case GL_FLOAT_MAT3: return 9;
    case GL_FLOAT_MAT4: return 16;
    default: return 1;
  }
}

static void set_one(GLint loc, GLenum type, const float *v) {
  switch (type) {
    case GL_FLOAT: glUniform1f(loc, v[0]); break;
    case GL_FLOAT_VEC2: glUniform2f(loc, v[0], v[1]); break;
    case GL_FLOAT_VEC3: glUniform3f(loc, v[0], v[1], v[2]); break;
    case GL_FLOAT_VEC4: glUniform4f(loc, v[0], v[1], v[2], v[3]); break;
    case GL_FLOAT_MAT4: glUniformMatrix4fv(loc, 1, GL_FALSE, v); break;
    case GL_INT: case GL_BOOL: case GL_SAMPLER_2D: case GL_SAMPLER_CUBE:
      glUniform1i(loc, (GLint)v[0]); break;
    default: break;
  }
}

// values: count elements of src_comps floats each
static void fixup(const char *func, GLint loc, int src_comps, GLsizei count, const float *values) {
  GLenum type;
  GLint index;
  if (!lookup(loc, &type, &index)) {
    if (s_fix_logs++ < MAX_FIX_LOGS)
      debugPrintf("uniform fix: %s at location %d: no matching uniform in current program\n", func, loc);
    return;
  }
  const int dst = components(type);
  float tmp[16];
  for (GLsizei e = 0; e < count; ++e) {
    memset(tmp, 0, sizeof(tmp));
    const int n = src_comps < dst ? src_comps : dst;
    memcpy(tmp, values + e * src_comps, n * sizeof(float));
    if (dst == 4 && src_comps == 3) tmp[3] = 1.0f;
    set_one(loc + e, type, tmp);
  }
  if (s_fix_logs++ < MAX_FIX_LOGS)
    debugPrintf("uniform fix: %s (%d comps x %d) -> uniform type 0x%x at location %d\n", func, src_comps, (int)count, type, loc);
}

int gl_frame_tracing(void);
#define TRACE(name, loc, ...) do { if (gl_frame_tracing()) gl_trace_check(name, loc, 1, __VA_ARGS__); } while (0)

static int s_other_logs;

static int rejected(void) {
  const GLenum err = glGetError();
  if (err != GL_NO_ERROR && err != GL_INVALID_OPERATION && config.debug_log && s_other_logs++ < MAX_FIX_LOGS)
    debugPrintf("uniform call error 0x%04x\n", err);
  return err == GL_INVALID_OPERATION;
}

void glUniform1f_fix(GLint loc, GLfloat v0) {
  glUniform1f(loc, v0);
  if (rejected()) fixup("glUniform1f", loc, 1, 1, &v0);
  TRACE("glUniform1f", loc, "%d, %g", loc, v0);
}

void glUniform1fv_fix(GLint loc, GLsizei count, const GLfloat *v) {
  glUniform1fv(loc, count, v);
  if (rejected()) fixup("glUniform1fv", loc, 1, count, v);
  TRACE("glUniform1fv", loc, "%d, %d, %g", loc, count, v[0]);
}

void glUniform1i_fix(GLint loc, GLint v0) {
  glUniform1i(loc, v0);
  if (rejected()) {
    const float f = (float)v0;
    fixup("glUniform1i", loc, 1, 1, &f);
  }
  TRACE("glUniform1i", loc, "%d, %d", loc, v0);
}

void glUniform2fv_fix(GLint loc, GLsizei count, const GLfloat *v) {
  glUniform2fv(loc, count, v);
  if (rejected()) fixup("glUniform2fv", loc, 2, count, v);
  TRACE("glUniform2fv", loc, "%d, %d, %g %g", loc, count, v[0], v[1]);
}

void glUniform3f_fix(GLint loc, GLfloat v0, GLfloat v1, GLfloat v2) {
  glUniform3f(loc, v0, v1, v2);
  if (rejected()) {
    const float v[3] = { v0, v1, v2 };
    fixup("glUniform3f", loc, 3, 1, v);
  }
  TRACE("glUniform3f", loc, "%d, %g %g %g", loc, v0, v1, v2);
}

void glUniform3fv_fix(GLint loc, GLsizei count, const GLfloat *v) {
  glUniform3fv(loc, count, v);
  if (rejected()) fixup("glUniform3fv", loc, 3, count, v);
  TRACE("glUniform3fv", loc, "%d, %d, %g %g %g", loc, count, v[0], v[1], v[2]);
}

void glUniform4fv_fix(GLint loc, GLsizei count, const GLfloat *v) {
  glUniform4fv(loc, count, v);
  if (rejected()) fixup("glUniform4fv", loc, 4, count, v);
  TRACE("glUniform4fv", loc, "%d, %d, %g %g %g %g", loc, count, v[0], v[1], v[2], v[3]);
}

void glUniformMatrix4fv_fix(GLint loc, GLsizei count, GLboolean transpose, const GLfloat *v) {
  glUniformMatrix4fv(loc, count, transpose, v);
  if (rejected()) fixup("glUniformMatrix4fv", loc, 16, count, v);
  TRACE("glUniformMatrix4fv", loc, "%d, %d, %d, [%g %g %g %g | %g %g %g %g]", loc, count, transpose, v[0], v[1], v[2], v[3], v[12], v[13], v[14], v[15]);
}
