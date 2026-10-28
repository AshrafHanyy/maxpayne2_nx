/* fake_jni.h -- minimal Java VM emulation for the game's JNI calls
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __FAKE_JNI_H__
#define __FAKE_JNI_H__

#include <stdint.h>

typedef struct jobj jobj;
typedef jobj *jobject;

typedef union {
  uint8_t z;
  int8_t b;
  uint16_t c;
  int16_t s;
  int32_t i;
  int64_t j;
  float f;
  double d;
  jobject l;
} jvalue;

enum {
  JK_CLASS = 1,
  JK_OBJECT,
  JK_STRING,
  JK_OBJ_ARRAY,
  JK_PRIM_ARRAY,
};

struct jobj {
  int kind;
  int refs;
  int persistent;  // never freed by DeleteLocalRef/DeleteGlobalRef
  jobj *cls;       // class of an instance; NULL for classes
  char *name;      // class name or UTF-8 string contents
  int length;      // array length
  int elem_size;   // primitive array element size
  void *elems;     // array storage
  void *user;      // per-object data for our Java classes
};

typedef struct jmethod jmethod;
typedef jvalue (*jhandler)(jmethod *m, jobject self, const jvalue *args);

struct jmethod {
  jobj *cls;
  char name[64];
  char sig[160];
  int is_static;
  jhandler fn;
  jmethod *next;
};

typedef struct jfield {
  jobj *cls;
  char name[64];
  char sig[64];
  struct jfield *next;
} jfield;

typedef jvalue (*jfield_getter)(jfield *f, jobject self);

// the env/vm pointers handed to the game (JNIEnv* and JavaVM*)
extern void *jni_env;
extern void *jni_vm;

void jni_init(void);

// registration of our Java class implementations
void jni_register_method(const char *cls, const char *name, jhandler fn);
void jni_register_field_getter(jfield_getter fn);

jobject jni_find_class(const char *name);
jobject jni_new_object(const char *cls);
jobject jni_new_string(const char *s, int persistent);
jobject jni_new_object_array(int len, jobject *elems);

#endif
