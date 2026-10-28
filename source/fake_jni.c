/* fake_jni.c -- minimal Java VM emulation for the game's JNI calls
 *
 * Implements the JNINativeInterface and JNIInvokeInterface function tables.
 * Method calls are dispatched by "class.name" to handlers registered from
 * java_classes.c; anything unknown is logged and returns zero.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "fake_jni.h"
#include "util.h"

#define JNI_NUM_FUNCS 233
#define JNI_VERSION_1_6 0x00010006

static void *s_env_table[JNI_NUM_FUNCS];
static void *s_vm_table[8];
static struct { void **functions; } s_env = { s_env_table };
static struct { void **functions; } s_vm = { s_vm_table };

void *jni_env = &s_env;
void *jni_vm = &s_vm;

static Mutex s_lock;
static jobj *s_classes[64];
static int s_num_classes;
static jmethod *s_methods;
static jfield *s_fields;
static jfield_getter s_field_getter;

typedef struct {
  char key[160];
  jhandler fn;
} handler_entry;

static handler_entry s_handlers[128];
static int s_num_handlers;

void jni_register_method(const char *cls, const char *name, jhandler fn) {
  if (s_num_handlers >= (int)(sizeof(s_handlers) / sizeof(*s_handlers))) return;
  snprintf(s_handlers[s_num_handlers].key, sizeof(s_handlers[0].key), "%s.%s", cls, name);
  s_handlers[s_num_handlers].fn = fn;
  ++s_num_handlers;
}

void jni_register_field_getter(jfield_getter fn) {
  s_field_getter = fn;
}

static jvalue default_handler(jmethod *m, jobject self, const jvalue *args) {
  debugPrintf("JNI: unhandled call %s.%s%s\n", m->cls ? m->cls->name : "?", m->name, m->sig);
  return (jvalue){ .j = 0 };
}

// objects

static jobj *obj_new(int kind, jobj *cls, int persistent) {
  jobj *o = calloc(1, sizeof(*o));
  o->kind = kind;
  o->refs = 1;
  o->cls = cls;
  o->persistent = persistent;
  return o;
}

static void obj_release(jobj *o) {
  if (!o || o->persistent || o->kind == JK_CLASS) return;
  if (--o->refs > 0) return;
  if (o->kind == JK_STRING) free(o->name);
  free(o->elems);
  free(o);
}

jobject jni_find_class(const char *name) {
  mutexLock(&s_lock);
  for (int i = 0; i < s_num_classes; ++i) {
    if (!strcmp(s_classes[i]->name, name)) {
      mutexUnlock(&s_lock);
      return s_classes[i];
    }
  }
  jobj *c = obj_new(JK_CLASS, NULL, 1);
  c->name = strdup(name);
  if (s_num_classes < (int)(sizeof(s_classes) / sizeof(*s_classes)))
    s_classes[s_num_classes++] = c;
  mutexUnlock(&s_lock);
  return c;
}

jobject jni_new_object(const char *cls) {
  return obj_new(JK_OBJECT, jni_find_class(cls), 1);
}

jobject jni_new_string(const char *s, int persistent) {
  if (!s) return NULL;
  jobj *o = obj_new(JK_STRING, jni_find_class("java/lang/String"), persistent);
  o->name = strdup(s);
  return o;
}

jobject jni_new_object_array(int len, jobject *elems) {
  jobj *o = obj_new(JK_OBJ_ARRAY, jni_find_class("[Ljava/lang/Object;"), 1);
  o->length = len;
  o->elem_size = sizeof(jobject);
  o->elems = calloc(len ? len : 1, sizeof(jobject));
  if (elems) memcpy(o->elems, elems, len * sizeof(jobject));
  return o;
}

static jmethod *get_method(jobj *cls, const char *name, const char *sig, int is_static) {
  mutexLock(&s_lock);
  for (jmethod *m = s_methods; m; m = m->next) {
    if (m->cls == cls && m->is_static == is_static && !strcmp(m->name, name) && !strcmp(m->sig, sig)) {
      mutexUnlock(&s_lock);
      return m;
    }
  }
  jmethod *m = calloc(1, sizeof(*m));
  m->cls = cls;
  m->is_static = is_static;
  strncpy(m->name, name, sizeof(m->name) - 1);
  strncpy(m->sig, sig, sizeof(m->sig) - 1);
  m->fn = default_handler;
  char key[160];
  snprintf(key, sizeof(key), "%s.%s", cls ? cls->name : "?", name);
  for (int i = 0; i < s_num_handlers; ++i)
    if (!strcmp(s_handlers[i].key, key)) m->fn = s_handlers[i].fn;
  m->next = s_methods;
  s_methods = m;
  mutexUnlock(&s_lock);
  if (m->fn == default_handler)
    debugPrintf("JNI: no handler for %s%s (will return 0)\n", key, sig);
  return m;
}

// argument unpacking according to the method signature

#define MAX_ARGS 16

static const char *skip_type(const char *p) {
  while (*p == '[') ++p;
  if (*p == 'L') {
    while (*p && *p != ';') ++p;
  }
  return *p ? p + 1 : p;
}

static int args_from_va(const jmethod *m, jvalue *out, va_list ap) {
  const char *p = strchr(m->sig, '(');
  int n = 0;
  if (!p) return 0;
  ++p;
  while (*p && *p != ')' && n < MAX_ARGS) {
    switch (*p) {
      case 'Z': out[n].z = (uint8_t)va_arg(ap, int); break;
      case 'B': out[n].b = (int8_t)va_arg(ap, int); break;
      case 'C': out[n].c = (uint16_t)va_arg(ap, int); break;
      case 'S': out[n].s = (int16_t)va_arg(ap, int); break;
      case 'I': out[n].i = va_arg(ap, int32_t); break;
      case 'J': out[n].j = va_arg(ap, int64_t); break;
      case 'F': out[n].f = (float)va_arg(ap, double); break;
      case 'D': out[n].d = va_arg(ap, double); break;
      default: out[n].l = va_arg(ap, jobject); break;
    }
    p = skip_type(p);
    ++n;
  }
  return n;
}

static jvalue invoke_v(jobject self, jmethod *m, va_list ap) {
  jvalue args[MAX_ARGS] = { 0 };
  if (!m) return (jvalue){ .j = 0 };
  args_from_va(m, args, ap);
  return m->fn(m, self, args);
}

static jvalue invoke_a(jobject self, jmethod *m, const jvalue *args) {
  jvalue empty[MAX_ARGS] = { 0 };
  if (!m) return (jvalue){ .j = 0 };
  return m->fn(m, self, args ? args : empty);
}

// Call<Type>Method family: 10 return types x (virtual, nonvirtual, static) x (..., V, A)

#define CALL_FAMILY(Type, ctype, member) \
  static ctype Call##Type##Method(void *env, jobject o, jmethod *m, ...) { \
    va_list ap; va_start(ap, m); jvalue r = invoke_v(o, m, ap); va_end(ap); return (ctype)r.member; } \
  static ctype Call##Type##MethodV(void *env, jobject o, jmethod *m, va_list ap) { \
    jvalue r = invoke_v(o, m, ap); return (ctype)r.member; } \
  static ctype Call##Type##MethodA(void *env, jobject o, jmethod *m, const jvalue *a) { \
    jvalue r = invoke_a(o, m, a); return (ctype)r.member; } \
  static ctype CallNonvirtual##Type##Method(void *env, jobject o, jobject c, jmethod *m, ...) { \
    va_list ap; va_start(ap, m); jvalue r = invoke_v(o, m, ap); va_end(ap); return (ctype)r.member; } \
  static ctype CallNonvirtual##Type##MethodV(void *env, jobject o, jobject c, jmethod *m, va_list ap) { \
    jvalue r = invoke_v(o, m, ap); return (ctype)r.member; } \
  static ctype CallNonvirtual##Type##MethodA(void *env, jobject o, jobject c, jmethod *m, const jvalue *a) { \
    jvalue r = invoke_a(o, m, a); return (ctype)r.member; } \
  static ctype CallStatic##Type##Method(void *env, jobject c, jmethod *m, ...) { \
    va_list ap; va_start(ap, m); jvalue r = invoke_v(c, m, ap); va_end(ap); return (ctype)r.member; } \
  static ctype CallStatic##Type##MethodV(void *env, jobject c, jmethod *m, va_list ap) { \
    jvalue r = invoke_v(c, m, ap); return (ctype)r.member; } \
  static ctype CallStatic##Type##MethodA(void *env, jobject c, jmethod *m, const jvalue *a) { \
    jvalue r = invoke_a(c, m, a); return (ctype)r.member; }

CALL_FAMILY(Object, jobject, l)
CALL_FAMILY(Boolean, uint8_t, z)
CALL_FAMILY(Byte, int8_t, b)
CALL_FAMILY(Char, uint16_t, c)
CALL_FAMILY(Short, int16_t, s)
CALL_FAMILY(Int, int32_t, i)
CALL_FAMILY(Long, int64_t, j)
CALL_FAMILY(Float, float, f)
CALL_FAMILY(Double, double, d)

static void CallVoidMethod(void *env, jobject o, jmethod *m, ...) {
  va_list ap; va_start(ap, m); invoke_v(o, m, ap); va_end(ap); }
static void CallVoidMethodV(void *env, jobject o, jmethod *m, va_list ap) { invoke_v(o, m, ap); }
static void CallVoidMethodA(void *env, jobject o, jmethod *m, const jvalue *a) { invoke_a(o, m, a); }
static void CallNonvirtualVoidMethod(void *env, jobject o, jobject c, jmethod *m, ...) {
  va_list ap; va_start(ap, m); invoke_v(o, m, ap); va_end(ap); }
static void CallNonvirtualVoidMethodV(void *env, jobject o, jobject c, jmethod *m, va_list ap) { invoke_v(o, m, ap); }
static void CallNonvirtualVoidMethodA(void *env, jobject o, jobject c, jmethod *m, const jvalue *a) { invoke_a(o, m, a); }
static void CallStaticVoidMethod(void *env, jobject c, jmethod *m, ...) {
  va_list ap; va_start(ap, m); invoke_v(c, m, ap); va_end(ap); }
static void CallStaticVoidMethodV(void *env, jobject c, jmethod *m, va_list ap) { invoke_v(c, m, ap); }
static void CallStaticVoidMethodA(void *env, jobject c, jmethod *m, const jvalue *a) { invoke_a(c, m, a); }

// fields

static jfield *get_field(jobj *cls, const char *name, const char *sig) {
  mutexLock(&s_lock);
  for (jfield *f = s_fields; f; f = f->next) {
    if (f->cls == cls && !strcmp(f->name, name)) {
      mutexUnlock(&s_lock);
      return f;
    }
  }
  jfield *f = calloc(1, sizeof(*f));
  f->cls = cls;
  strncpy(f->name, name, sizeof(f->name) - 1);
  strncpy(f->sig, sig, sizeof(f->sig) - 1);
  f->next = s_fields;
  s_fields = f;
  mutexUnlock(&s_lock);
  return f;
}

static jvalue field_value(jfield *f, jobject self) {
  if (f && s_field_getter) return s_field_getter(f, self);
  return (jvalue){ .j = 0 };
}

#define FIELD_GET(Type, ctype, member) \
  static ctype Get##Type##Field(void *env, jobject o, jfield *f) { return (ctype)field_value(f, o).member; } \
  static ctype GetStatic##Type##Field(void *env, jobject c, jfield *f) { return (ctype)field_value(f, c).member; }

FIELD_GET(Object, jobject, l)
FIELD_GET(Boolean, uint8_t, z)
FIELD_GET(Byte, int8_t, b)
FIELD_GET(Char, uint16_t, c)
FIELD_GET(Short, int16_t, s)
FIELD_GET(Int, int32_t, i)
FIELD_GET(Long, int64_t, j)
FIELD_GET(Float, float, f)
FIELD_GET(Double, double, d)

static void SetFieldNop(void) { }

// basic env functions

static int32_t GetVersion(void *env) { return JNI_VERSION_1_6; }
static jobject FindClass(void *env, const char *name) { return jni_find_class(name); }
static jobject ExceptionOccurred(void *env) { return NULL; }
static void ExceptionNop(void *env) { }
static uint8_t ExceptionCheck(void *env) { return 0; }
static int32_t ThrowNew(void *env, jobject c, const char *msg) {
  debugPrintf("JNI: ThrowNew(%s): %s\n", c ? c->name : "?", msg ? msg : "");
  return 0;
}
static void FatalError(void *env, const char *msg) {
  debugPrintf("JNI FatalError: %s\n", msg);
}
static int32_t ret0_int(void) { return 0; }
static jobject PopLocalFrame(void *env, jobject res) { return res; }
static jobject NewRef(void *env, jobject o) {
  if (o && !o->persistent && o->kind != JK_CLASS) ++o->refs;
  return o;
}
static void DeleteRef(void *env, jobject o) { obj_release(o); }
static uint8_t IsSameObject(void *env, jobject a, jobject b) { return a == b; }
static jobject AllocObject(void *env, jobject c) { return obj_new(JK_OBJECT, c, 0); }
static jobject NewObject(void *env, jobject c, jmethod *m, ...) { return obj_new(JK_OBJECT, c, 0); }
static jobject NewObjectV(void *env, jobject c, jmethod *m, va_list ap) { return obj_new(JK_OBJECT, c, 0); }
static jobject NewObjectA(void *env, jobject c, jmethod *m, const jvalue *a) { return obj_new(JK_OBJECT, c, 0); }
static jobject GetObjectClass(void *env, jobject o) {
  if (!o) return NULL;
  return o->kind == JK_CLASS ? jni_find_class("java/lang/Class") : o->cls;
}
static uint8_t IsInstanceOf(void *env, jobject o, jobject c) { return 1; }
static jmethod *GetMethodID(void *env, jobject c, const char *name, const char *sig) { return get_method(c, name, sig, 0); }
static jmethod *GetStaticMethodID(void *env, jobject c, const char *name, const char *sig) { return get_method(c, name, sig, 1); }
static jfield *GetFieldID(void *env, jobject c, const char *name, const char *sig) { return get_field(c, name, sig); }

// strings

static jobject NewStringUTF(void *env, const char *s) { return jni_new_string(s, 0); }
static int32_t GetStringUTFLength(void *env, jobject s) { return (s && s->name) ? (int32_t)strlen(s->name) : 0; }
static const char *GetStringUTFChars(void *env, jobject s, uint8_t *is_copy) {
  if (is_copy) *is_copy = 0;
  return (s && s->name) ? s->name : "";
}
static void ReleaseStringUTFChars(void *env, jobject s, const char *chars) { }
static jobject NewString(void *env, const uint16_t *chars, int32_t len) {
  char *tmp = malloc(len + 1);
  for (int i = 0; i < len; ++i) tmp[i] = chars[i] < 0x80 ? (char)chars[i] : '?';
  tmp[len] = 0;
  jobject o = jni_new_string(tmp, 0);
  free(tmp);
  return o;
}
static const uint16_t *GetStringChars(void *env, jobject s, uint8_t *is_copy) {
  const char *src = (s && s->name) ? s->name : "";
  const size_t len = strlen(src);
  uint16_t *out = malloc((len + 1) * sizeof(uint16_t));
  for (size_t i = 0; i <= len; ++i) out[i] = (uint8_t)src[i];
  if (is_copy) *is_copy = 1;
  return out;
}
static void ReleaseStringChars(void *env, jobject s, const uint16_t *chars) { free((void *)chars); }
static void GetStringUTFRegion(void *env, jobject s, int32_t start, int32_t len, char *buf) {
  memcpy(buf, s->name + start, len);
  buf[len] = 0;
}
static void GetStringRegion(void *env, jobject s, int32_t start, int32_t len, uint16_t *buf) {
  for (int i = 0; i < len; ++i) buf[i] = (uint8_t)s->name[start + i];
}

// arrays

static int32_t GetArrayLength(void *env, jobject a) { return a ? a->length : 0; }
static jobject NewObjectArray(void *env, int32_t len, jobject c, jobject init) {
  jobj *o = obj_new(JK_OBJ_ARRAY, c, 0);
  o->length = len;
  o->elem_size = sizeof(jobject);
  o->elems = calloc(len ? len : 1, sizeof(jobject));
  for (int i = 0; i < len; ++i) ((jobject *)o->elems)[i] = init;
  return o;
}
static jobject GetObjectArrayElement(void *env, jobject a, int32_t i) {
  return (a && i >= 0 && i < a->length) ? ((jobject *)a->elems)[i] : NULL;
}
static void SetObjectArrayElement(void *env, jobject a, int32_t i, jobject v) {
  if (a && i >= 0 && i < a->length) ((jobject *)a->elems)[i] = v;
}

static jobject new_prim_array(int32_t len, int elem_size) {
  jobj *o = obj_new(JK_PRIM_ARRAY, jni_find_class("[prim"), 0);
  o->length = len;
  o->elem_size = elem_size;
  o->elems = calloc(len ? len : 1, elem_size);
  return o;
}

#define PRIM_ARRAY(Type, size) \
  static jobject New##Type##Array(void *env, int32_t len) { return new_prim_array(len, size); }

PRIM_ARRAY(Boolean, 1)
PRIM_ARRAY(Byte, 1)
PRIM_ARRAY(Char, 2)
PRIM_ARRAY(Short, 2)
PRIM_ARRAY(Int, 4)
PRIM_ARRAY(Long, 8)
PRIM_ARRAY(Float, 4)
PRIM_ARRAY(Double, 8)

static void *GetArrayElements(void *env, jobject a, uint8_t *is_copy) {
  if (is_copy) *is_copy = 0;
  return a ? a->elems : NULL;
}
static void ReleaseArrayElements(void *env, jobject a, void *elems, int32_t mode) { }
static void GetArrayRegion(void *env, jobject a, int32_t start, int32_t len, void *buf) {
  if (a) memcpy(buf, (uint8_t *)a->elems + start * a->elem_size, len * a->elem_size);
}
static void SetArrayRegion(void *env, jobject a, int32_t start, int32_t len, const void *buf) {
  if (a) memcpy((uint8_t *)a->elems + start * a->elem_size, buf, len * a->elem_size);
}
static void *GetPrimitiveArrayCritical(void *env, jobject a, uint8_t *is_copy) { return GetArrayElements(env, a, is_copy); }

// misc

static int32_t RegisterNatives(void *env, jobject c, const void *methods, int32_t n) {
  debugPrintf("JNI: RegisterNatives(%s, %d) ignored\n", c ? c->name : "?", n);
  return 0;
}
static int32_t GetJavaVM(void *env, void **vm) { *vm = jni_vm; return 0; }
static int32_t GetObjectRefType(void *env, jobject o) { return 1; }

// JavaVM

static int32_t vm_GetEnv(void *vm, void **env, int32_t version) { *env = jni_env; return 0; }
static int32_t vm_AttachCurrentThread(void *vm, void **env, void *args) { *env = jni_env; return 0; }

// unimplemented slots: each logs its own index

static void jni_unimpl(int n) {
  debugPrintf("JNI: unimplemented JNIEnv function #%d called\n", n);
}

#define STUB(n) static long jni_stub_##n(void) { jni_unimpl(n); return 0; }
#define STUBREF(n) (void *)jni_stub_##n,
#define D10(m, p) m(p##0) m(p##1) m(p##2) m(p##3) m(p##4) m(p##5) m(p##6) m(p##7) m(p##8) m(p##9)
#define ALL(m) D10(m, ) D10(m, 1) D10(m, 2) D10(m, 3) D10(m, 4) D10(m, 5) D10(m, 6) D10(m, 7) \
  D10(m, 8) D10(m, 9) D10(m, 10) D10(m, 11) D10(m, 12) D10(m, 13) D10(m, 14) D10(m, 15) D10(m, 16) \
  D10(m, 17) D10(m, 18) D10(m, 19) D10(m, 20) D10(m, 21) D10(m, 22) m(230) m(231) m(232)

ALL(STUB)
static void *const s_stubs[JNI_NUM_FUNCS] = { ALL(STUBREF) };

// slot indices (jni.h order)
enum {
  J_GetVersion = 4, J_FindClass = 6, J_ThrowNew = 14, J_ExceptionOccurred = 15, J_ExceptionDescribe = 16,
  J_ExceptionClear = 17, J_FatalError = 18, J_PushLocalFrame = 19, J_PopLocalFrame = 20,
  J_NewGlobalRef = 21, J_DeleteGlobalRef = 22, J_DeleteLocalRef = 23, J_IsSameObject = 24,
  J_NewLocalRef = 25, J_EnsureLocalCapacity = 26, J_AllocObject = 27, J_NewObject = 28,
  J_NewObjectV = 29, J_NewObjectA = 30, J_GetObjectClass = 31, J_IsInstanceOf = 32, J_GetMethodID = 33,
  J_CallObjectMethod = 34, J_CallNonvirtualObjectMethod = 64, J_GetFieldID = 94, J_GetObjectField = 95,
  J_SetObjectField = 104, J_GetStaticMethodID = 113, J_CallStaticObjectMethod = 114,
  J_GetStaticFieldID = 144, J_GetStaticObjectField = 145, J_SetStaticObjectField = 154,
  J_NewString = 163, J_GetStringLength = 164, J_GetStringChars = 165, J_ReleaseStringChars = 166,
  J_NewStringUTF = 167, J_GetStringUTFLength = 168, J_GetStringUTFChars = 169,
  J_ReleaseStringUTFChars = 170, J_GetArrayLength = 171, J_NewObjectArray = 172,
  J_GetObjectArrayElement = 173, J_SetObjectArrayElement = 174, J_NewBooleanArray = 175,
  J_GetBooleanArrayElements = 183, J_ReleaseBooleanArrayElements = 191, J_GetBooleanArrayRegion = 199,
  J_SetBooleanArrayRegion = 207, J_RegisterNatives = 215, J_UnregisterNatives = 216,
  J_MonitorEnter = 217, J_MonitorExit = 218, J_GetJavaVM = 219, J_GetStringRegion = 220,
  J_GetStringUTFRegion = 221, J_GetPrimitiveArrayCritical = 222, J_ReleasePrimitiveArrayCritical = 223,
  J_GetStringCritical = 224, J_ReleaseStringCritical = 225, J_NewWeakGlobalRef = 226,
  J_DeleteWeakGlobalRef = 227, J_ExceptionCheck = 228, J_GetObjectRefType = 232,
};

#define SET_CALLS(base, Type) \
  t[base + 0] = Call##Type##Method; t[base + 1] = Call##Type##MethodV; t[base + 2] = Call##Type##MethodA;
#define SET_NV(base, Type) \
  t[base + 0] = CallNonvirtual##Type##Method; t[base + 1] = CallNonvirtual##Type##MethodV; t[base + 2] = CallNonvirtual##Type##MethodA;
#define SET_ST(base, Type) \
  t[base + 0] = CallStatic##Type##Method; t[base + 1] = CallStatic##Type##MethodV; t[base + 2] = CallStatic##Type##MethodA;

void jni_init(void) {
  void **t = s_env_table;
  memcpy(t, s_stubs, sizeof(s_stubs));

  t[J_GetVersion] = GetVersion;
  t[J_FindClass] = FindClass;
  t[J_ThrowNew] = ThrowNew;
  t[13] = ret0_int; // Throw
  t[J_ExceptionOccurred] = ExceptionOccurred;
  t[J_ExceptionDescribe] = ExceptionNop;
  t[J_ExceptionClear] = ExceptionNop;
  t[J_FatalError] = FatalError;
  t[J_PushLocalFrame] = ret0_int;
  t[J_PopLocalFrame] = PopLocalFrame;
  t[J_NewGlobalRef] = NewRef;
  t[J_DeleteGlobalRef] = DeleteRef;
  t[J_DeleteLocalRef] = DeleteRef;
  t[J_IsSameObject] = IsSameObject;
  t[J_NewLocalRef] = NewRef;
  t[J_EnsureLocalCapacity] = ret0_int;
  t[J_AllocObject] = AllocObject;
  t[J_NewObject] = NewObject;
  t[J_NewObjectV] = NewObjectV;
  t[J_NewObjectA] = NewObjectA;
  t[J_GetObjectClass] = GetObjectClass;
  t[J_IsInstanceOf] = IsInstanceOf;
  t[J_GetMethodID] = GetMethodID;

  // order: Object Boolean Byte Char Short Int Long Float Double Void
  SET_CALLS(34, Object) SET_CALLS(37, Boolean) SET_CALLS(40, Byte) SET_CALLS(43, Char) SET_CALLS(46, Short)
  SET_CALLS(49, Int) SET_CALLS(52, Long) SET_CALLS(55, Float) SET_CALLS(58, Double) SET_CALLS(61, Void)
  SET_NV(64, Object) SET_NV(67, Boolean) SET_NV(70, Byte) SET_NV(73, Char) SET_NV(76, Short)
  SET_NV(79, Int) SET_NV(82, Long) SET_NV(85, Float) SET_NV(88, Double) SET_NV(91, Void)

  t[J_GetFieldID] = GetFieldID;
  t[95] = GetObjectField; t[96] = GetBooleanField; t[97] = GetByteField; t[98] = GetCharField;
  t[99] = GetShortField; t[100] = GetIntField; t[101] = GetLongField; t[102] = GetFloatField; t[103] = GetDoubleField;
  for (int i = 104; i <= 112; ++i) t[i] = SetFieldNop;

  t[J_GetStaticMethodID] = GetStaticMethodID;
  SET_ST(114, Object) SET_ST(117, Boolean) SET_ST(120, Byte) SET_ST(123, Char) SET_ST(126, Short)
  SET_ST(129, Int) SET_ST(132, Long) SET_ST(135, Float) SET_ST(138, Double) SET_ST(141, Void)

  t[J_GetStaticFieldID] = GetFieldID;
  t[145] = GetStaticObjectField; t[146] = GetStaticBooleanField; t[147] = GetStaticByteField;
  t[148] = GetStaticCharField; t[149] = GetStaticShortField; t[150] = GetStaticIntField;
  t[151] = GetStaticLongField; t[152] = GetStaticFloatField; t[153] = GetStaticDoubleField;
  for (int i = 154; i <= 162; ++i) t[i] = SetFieldNop;

  t[J_NewString] = NewString;
  t[J_GetStringLength] = GetStringUTFLength;
  t[J_GetStringChars] = GetStringChars;
  t[J_ReleaseStringChars] = ReleaseStringChars;
  t[J_NewStringUTF] = NewStringUTF;
  t[J_GetStringUTFLength] = GetStringUTFLength;
  t[J_GetStringUTFChars] = GetStringUTFChars;
  t[J_ReleaseStringUTFChars] = ReleaseStringUTFChars;
  t[J_GetArrayLength] = GetArrayLength;
  t[J_NewObjectArray] = NewObjectArray;
  t[J_GetObjectArrayElement] = GetObjectArrayElement;
  t[J_SetObjectArrayElement] = SetObjectArrayElement;
  t[175] = NewBooleanArray; t[176] = NewByteArray; t[177] = NewCharArray; t[178] = NewShortArray;
  t[179] = NewIntArray; t[180] = NewLongArray; t[181] = NewFloatArray; t[182] = NewDoubleArray;
  for (int i = 0; i < 8; ++i) {
    t[J_GetBooleanArrayElements + i] = GetArrayElements;
    t[J_ReleaseBooleanArrayElements + i] = ReleaseArrayElements;
    t[J_GetBooleanArrayRegion + i] = GetArrayRegion;
    t[J_SetBooleanArrayRegion + i] = SetArrayRegion;
  }
  t[J_RegisterNatives] = RegisterNatives;
  t[J_UnregisterNatives] = ret0_int;
  t[J_MonitorEnter] = ret0_int;
  t[J_MonitorExit] = ret0_int;
  t[J_GetJavaVM] = GetJavaVM;
  t[J_GetStringRegion] = GetStringRegion;
  t[J_GetStringUTFRegion] = GetStringUTFRegion;
  t[J_GetPrimitiveArrayCritical] = GetPrimitiveArrayCritical;
  t[J_ReleasePrimitiveArrayCritical] = ReleaseArrayElements;
  t[J_GetStringCritical] = GetStringChars;
  t[J_ReleaseStringCritical] = ReleaseStringChars;
  t[J_NewWeakGlobalRef] = NewRef;
  t[J_DeleteWeakGlobalRef] = DeleteRef;
  t[J_ExceptionCheck] = ExceptionCheck;
  t[J_GetObjectRefType] = GetObjectRefType;

  for (int i = 0; i < 8; ++i) s_vm_table[i] = ret0_int;
  s_vm_table[4] = vm_AttachCurrentThread;
  s_vm_table[6] = vm_GetEnv;
  s_vm_table[7] = vm_AttachCurrentThread;
}
