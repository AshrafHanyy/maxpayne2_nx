/* java_classes.c -- our implementations of the game's Java-side classes
 *
 * GamePlatformServices: the up-call interface the native code uses.
 * NvUtil:               storage root and app-local key/value store.
 * DeviceInfo:           read via boolean fields.
 *
 * Asynchronous Java calls (Rockstar gate, playlist, HTTP) are answered by
 * queueing the matching GameNative callback and running it before the
 * next frame, just like the Java GameThread does.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <string.h>

#include "java_classes.h"
#include "config.h"
#include "util.h"

#define GPS_CLASS "com/rockstargames/oswrapper/GamePlatformServices"
#define NVUTIL_CLASS "com/nvidia/devtech/NvUtil"
#define APP_VERSION "2.1.131"
#define STORAGE_ROOT "."

void gl_trace_arm(float seconds);

GameNatives natives;

jobject obj_game_native_class;
jobject obj_platform_services;
jobject obj_device_info;
jobject obj_asset_manager;
jobject obj_surface;

static jobject s_nvutil;
static int s_quit = 0;
// ApplicationTick's startup sequence waits for the splash screen to become visible
static int s_splash_visible = 0;

// deferred callbacks

enum {
  CB_ROCKSTAR_INITIAL,
  CB_ROCKSTAR_GATE,
  CB_ROCKSTAR_SIGNOUT,
  CB_ROCKSTAR_CLOUD_DISABLED,
  CB_ROCKSTAR_DELETE_ACCOUNT,
  CB_HTTP_ERROR,
  CB_PLAYLIST_OPEN,
};

typedef struct {
  int type;
  int a;
} callback;

#define MAX_CALLBACKS 64
static callback s_cb[MAX_CALLBACKS];
static int s_cb_count;
static Mutex s_cb_lock;

static void queue_callback(int type, int a) {
  mutexLock(&s_cb_lock);
  if (s_cb_count < MAX_CALLBACKS)
    s_cb[s_cb_count++] = (callback){ type, a };
  mutexUnlock(&s_cb_lock);
}

void java_process_callbacks(void) {
  callback pending[MAX_CALLBACKS];
  mutexLock(&s_cb_lock);
  const int n = s_cb_count;
  memcpy(pending, s_cb, n * sizeof(callback));
  s_cb_count = 0;
  mutexUnlock(&s_cb_lock);

  void *env = jni_env;
  jobject cls = obj_game_native_class;
  for (int i = 0; i < n; ++i) {
    debugPrintf("callback %d (%d)\n", pending[i].type, pending[i].a);
    switch (pending[i].type) {
      case CB_ROCKSTAR_INITIAL: natives.OnRockstarInitialComplete(env, cls); break;
      case CB_ROCKSTAR_GATE: natives.OnRockstarGateComplete(env, cls, pending[i].a, 1); break;
      case CB_ROCKSTAR_SIGNOUT: natives.OnRockstarSignOutComplete(env, cls); break;
      case CB_ROCKSTAR_CLOUD_DISABLED: natives.OnRockstarCloudDisabledComplete(env, cls); break;
      case CB_ROCKSTAR_DELETE_ACCOUNT: natives.OnRockstarDeleteAccountComplete(env, cls); break;
      case CB_HTTP_ERROR: natives.OnHttpRequestError(env, cls, pending[i].a, -1); break;
      case CB_PLAYLIST_OPEN: natives.OnPlaylistOpenComplete(env, cls, 0, 0); break;
    }
  }
}

int java_quit_requested(void) {
  return s_quit;
}

// handlers

#define H(fn) static jvalue fn(jmethod *m, jobject self, const jvalue *args)
#define RET_VOID return (jvalue){ .j = 0 }
#define RET_INT(v) return (jvalue){ .i = (v) }
#define RET_BOOL(v) return (jvalue){ .z = (v) }
#define RET_OBJ(v) return (jvalue){ .l = (v) }

static const char *str_arg(jobject s) {
  return (s && s->name) ? s->name : "(null)";
}

H(gps_getAppVersion) { RET_OBJ(jni_new_string(APP_VERSION, 0)); }
H(gps_getDeviceLocale) { RET_INT(config.language); }
H(gps_quit) { debugPrintf("game requested quit\n"); s_quit = 1; RET_VOID; }
H(gps_false) { RET_BOOL(0); }
H(gps_zero) { RET_INT(0); }
H(gps_nop) { RET_VOID; }
H(gps_log_str) { debugPrintf("Java %s(%s)\n", m->name, args[0].l ? str_arg(args[0].l) : ""); RET_VOID; }

H(gps_showSplashScreen) { debugPrintf("showSplashScreen\n"); s_splash_visible = 1; RET_VOID; }
H(gps_hideSplashScreen) {
  debugPrintf("hideSplashScreen\n");
  s_splash_visible = 0;
  // capture one full frame of GL calls once gameplay is likely running
  if (config.gl_trace) gl_trace_arm(15.0f);
  RET_VOID;
}
H(gps_isSplashScreenVisible) { RET_BOOL(s_splash_visible); }

H(gps_playlistOpen) {
  debugPrintf("playlistOpen(%s)\n", str_arg(args[0].l));
  queue_callback(CB_PLAYLIST_OPEN, 0);
  RET_VOID;
}

H(gps_rockstarShowInitial) { queue_callback(CB_ROCKSTAR_INITIAL, 0); RET_VOID; }
H(gps_rockstarShowGate) { queue_callback(CB_ROCKSTAR_GATE, args[0].i); RET_VOID; }
H(gps_rockstarSignOut) { queue_callback(CB_ROCKSTAR_SIGNOUT, 0); RET_VOID; }
H(gps_rockstarShowCloudDisabled) { queue_callback(CB_ROCKSTAR_CLOUD_DISABLED, 0); RET_VOID; }
H(gps_rockstarDeleteAccount) { queue_callback(CB_ROCKSTAR_DELETE_ACCOUNT, 0); RET_VOID; }
H(gps_http) {
  debugPrintf("Java %s(%d, %s) -> error\n", m->name, args[0].i, str_arg(args[1].l));
  queue_callback(CB_HTTP_ERROR, args[0].i);
  RET_VOID;
}

// NvUtil key/value store

typedef struct kv {
  char *key, *value;
  struct kv *next;
} kv;

static kv *s_kv;

static kv *kv_find(const char *key) {
  for (kv *e = s_kv; e; e = e->next)
    if (!strcmp(e->key, key)) return e;
  return NULL;
}

static void kv_set(const char *key, const char *value) {
  kv *e = kv_find(key);
  if (!e) {
    e = calloc(1, sizeof(*e));
    e->key = strdup(key);
    e->next = s_kv;
    s_kv = e;
  } else {
    free(e->value);
  }
  e->value = strdup(value ? value : "");
}

H(nv_getInstance) { RET_OBJ(s_nvutil); }
H(nv_hasAppLocalValue) { RET_BOOL(kv_find(str_arg(args[0].l)) != NULL); }
H(nv_getAppLocalValue) {
  kv *e = kv_find(str_arg(args[0].l));
  debugPrintf("NvUtil.getAppLocalValue(%s) = %s\n", str_arg(args[0].l), e ? e->value : "(none)");
  RET_OBJ(e ? jni_new_string(e->value, 0) : NULL);
}
H(nv_setAppLocalValue) {
  kv_set(str_arg(args[0].l), args[1].l ? str_arg(args[1].l) : "");
  RET_VOID;
}

// DeviceInfo fields

static jvalue field_getter(jfield *f, jobject self) {
  jvalue v = { .j = 0 };
  if (!strcmp(f->name, "isPhone")) v.z = 0;
  else if (!strcmp(f->name, "hasTouchScreen")) v.z = config.touch_screen ? 1 : 0;
  else debugPrintf("JNI: unknown field %s.%s (%s)\n", f->cls ? f->cls->name : "?", f->name, f->sig);
  return v;
}

#define NATIVE(field, name) natives.field = (void *)so_symbol(game, "Java_com_rockstargames_oswrapper_GameNative_impl" name)

void java_init(so_module *game) {
  jni_init();

  static const struct { const char *name; jhandler fn; } gps[] = {
    { "getAppVersion", gps_getAppVersion },
    { "getDeviceLocale", gps_getDeviceLocale },
    { "quit", gps_quit },
    { "openLink", gps_log_str },
    { "showKeyboard", gps_nop },
    { "hasVibrator", gps_false },
    { "vibrate", gps_nop },
    { "showSplashScreen", gps_showSplashScreen },
    { "hideSplashScreen", gps_hideSplashScreen },
    { "isSplashScreenVisible", gps_isSplashScreenVisible },
    { "setSplashImage", gps_nop },
    { "setSplashText", gps_nop },
    { "playMovie", gps_log_str },
    { "stopMovie", gps_nop },
    { "pauseMovie", gps_nop },
    { "isMoviePlaying", gps_false },
    { "setMovieText", gps_nop },
    { "setMovieTextScale", gps_nop },
    { "playlistOpen", gps_playlistOpen },
    { "playlistPlay", gps_nop },
    { "playlistPause", gps_nop },
    { "playlistStop", gps_nop },
    { "playlistIsPlaying", gps_false },
    { "playlistCount", gps_zero },
    { "playlistSetVolume", gps_nop },
    { "rockstarShowInitial", gps_rockstarShowInitial },
    { "rockstarShowGate", gps_rockstarShowGate },
    { "rockstarSignIn", gps_nop },
    { "rockstarSignOut", gps_rockstarSignOut },
    { "rockstarFetchId", gps_nop },
    { "rockstarInTrial", gps_false },
    { "rockstarLogError", gps_log_str },
    { "rockstarLogBreadcrumb", gps_nop },
    { "rockstarSetLocalePriority", gps_nop },
    { "rockstarRequestReview", gps_nop },
    { "rockstarShowCloudDisabled", gps_rockstarShowCloudDisabled },
    { "rockstarDeleteAccount", gps_rockstarDeleteAccount },
    { "httpGet", gps_http },
    { "httpHead", gps_http },
    { "httpPost", gps_http },
    { "httpCancel", gps_nop },
  };
  for (size_t i = 0; i < sizeof(gps) / sizeof(*gps); ++i)
    jni_register_method(GPS_CLASS, gps[i].name, gps[i].fn);

  jni_register_method(NVUTIL_CLASS, "getInstance", nv_getInstance);
  jni_register_method(NVUTIL_CLASS, "hasAppLocalValue", nv_hasAppLocalValue);
  jni_register_method(NVUTIL_CLASS, "getAppLocalValue", nv_getAppLocalValue);
  jni_register_method(NVUTIL_CLASS, "getParameter", nv_getAppLocalValue);
  jni_register_method(NVUTIL_CLASS, "setAppLocalValue", nv_setAppLocalValue);
  jni_register_field_getter(field_getter);

  kv_set("STORAGE_ROOT", STORAGE_ROOT);
  kv_set("STORAGE_ROOT_BASE", STORAGE_ROOT);

  obj_game_native_class = jni_find_class("com/rockstargames/oswrapper/GameNative");
  obj_platform_services = jni_new_object(GPS_CLASS);
  obj_device_info = jni_new_object("com/rockstargames/oswrapper/DeviceInfo");
  obj_asset_manager = jni_new_object("android/content/res/AssetManager");
  obj_surface = jni_new_object("android/view/Surface");
  s_nvutil = jni_new_object(NVUTIL_CLASS);

  natives.JNI_OnLoad = (void *)so_symbol(game, "JNI_OnLoad");
  NATIVE(OnActivityCreated, "OnActivityCreated");
  NATIVE(OnInitialSetup, "OnInitialSetup");
  NATIVE(OnSurfaceCreated, "OnSurfaceCreated");
  NATIVE(OnSurfaceChanged, "OnSurfaceChanged");
  NATIVE(OnDrawFrame, "OnDrawFrame");
  NATIVE(OnPause, "OnPause");
  NATIVE(OnResume, "OnResume");
  NATIVE(OnGamepadConnected, "OnGamepadConnected");
  NATIVE(OnGamepadButtonDown, "OnGamepadButtonDown");
  NATIVE(OnGamepadButtonUp, "OnGamepadButtonUp");
  NATIVE(OnGamepadAxesChanged, "OnGamepadAxesChanged");
  NATIVE(OnTouchStart, "OnTouchStart");
  NATIVE(OnTouchMove, "OnTouchMove");
  NATIVE(OnTouchEnd, "OnTouchEnd");
  NATIVE(OnRockstarInitialComplete, "OnRockstarInitialComplete");
  NATIVE(OnRockstarGateComplete, "OnRockstarGateComplete");
  NATIVE(OnRockstarSignOutComplete, "OnRockstarSignOutComplete");
  NATIVE(OnRockstarCloudDisabledComplete, "OnRockstarCloudDisabledComplete");
  NATIVE(OnRockstarDeleteAccountComplete, "OnRockstarDeleteAccountComplete");
  NATIVE(OnHttpRequestError, "OnHttpRequestError");
  NATIVE(OnPlaylistOpenComplete, "OnPlaylistOpenComplete");
}
