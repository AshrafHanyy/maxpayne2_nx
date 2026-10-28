/* java_classes.h -- our implementations of the game's Java-side classes
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JAVA_CLASSES_H__
#define __JAVA_CLASSES_H__

#include "fake_jni.h"
#include "so_util.h"

// com.rockstargames.oswrapper.GameNative natives
typedef struct {
  int (*JNI_OnLoad)(void *vm, void *reserved);
  void (*OnActivityCreated)(void *env, jobject cls, jobject services, uint8_t flag);
  void (*OnInitialSetup)(void *env, jobject cls, jobject device_info, jobject assets, jobject names, jobject paths);
  void (*OnSurfaceCreated)(void *env, jobject cls);
  void (*OnSurfaceChanged)(void *env, jobject cls, jobject surface, int w, int h);
  void (*OnDrawFrame)(void *env, jobject cls, float dt);
  void (*OnPause)(void *env, jobject cls);
  void (*OnResume)(void *env, jobject cls);
  void (*OnGamepadConnected)(void *env, jobject cls, int dev);
  void (*OnGamepadButtonDown)(void *env, jobject cls, int dev, int button);
  void (*OnGamepadButtonUp)(void *env, jobject cls, int dev, int button);
  void (*OnGamepadAxesChanged)(void *env, jobject cls, int dev, float lx, float ly, float rx, float ry, float lt, float rt);
  void (*OnTouchStart)(void *env, jobject cls, int id, float x, float y);
  void (*OnTouchMove)(void *env, jobject cls, int id, float x, float y);
  void (*OnTouchEnd)(void *env, jobject cls, int id, float x, float y);
  void (*OnRockstarInitialComplete)(void *env, jobject cls);
  void (*OnRockstarGateComplete)(void *env, jobject cls, int gate, uint8_t ok);
  void (*OnRockstarSignOutComplete)(void *env, jobject cls);
  void (*OnRockstarCloudDisabledComplete)(void *env, jobject cls);
  void (*OnRockstarDeleteAccountComplete)(void *env, jobject cls);
  void (*OnHttpRequestError)(void *env, jobject cls, int id, int code);
  void (*OnPlaylistOpenComplete)(void *env, jobject cls, uint8_t ok, int count);
} GameNatives;

extern GameNatives natives;

// objects passed to the game
extern jobject obj_game_native_class;
extern jobject obj_platform_services;
extern jobject obj_device_info;
extern jobject obj_asset_manager;
extern jobject obj_surface;

void java_init(so_module *game);
void java_process_callbacks(void);
int java_quit_requested(void);

#endif
