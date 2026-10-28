/* gl_trace.h -- GL error tracing (see tools/gen_gl_trace.py)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __GL_TRACE_H__
#define __GL_TRACE_H__

typedef struct {
  const char *name;
  void *func;
} gl_trace_entry;

extern const gl_trace_entry gl_trace_table[];

// checks glGetError() after a call and logs failures with their arguments
void gl_trace_check(const char *func, int location, int is_uniform, const char *fmt, ...);

#endif
