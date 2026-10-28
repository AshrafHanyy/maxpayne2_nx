/* main.c -- Max Payne Mobile 2.x loader for Nintendo Switch
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 * Reworked for Max Payne Mobile 2.x (oswrapper lifecycle, fake JNI).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "hooks.h"
#include "imports.h"
#include "bionic_tls.h"
#include "java_classes.h"

#define MAX_FRAME_TIME 0.1f

void gl_debug_frame(unsigned long frame);
// the game loads levels and renders on the thread that drives it; give it a big stack
#define GAME_THREAD_STACK (16 * 1024 * 1024)

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

// replacement heap init: newlib gets everything except a region at the top for the .so modules
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  size_t so_size = (size_t)SO_REGION_MB * 1024 * 1024;
  if (so_size > size / 2) so_size = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_start = (char *)addr;
  fake_heap_end = (char *)addr + size - so_size;

  heap_so_base = fake_heap_end;
  heap_so_limit = so_size;
}

static void enter_data_dir(int argc, char *argv[]) {
  // run from the folder the .nro is in, if we can tell
  if (argc > 0 && argv[0] && strchr(argv[0], '/')) {
    char path[0x300];
    strncpy(path, argv[0], sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    char *slash = strrchr(path, '/');
    *slash = 0;
    if (chdir(path) == 0) return;
  }
  chdir(DEFAULT_DATA_DIR);
}

// openal-soft reads its settings from the file named in ALSOFT_CONF
static void setup_audio_config(void) {
  static const char *name = "alsoft.conf";
  struct stat st;
  if (stat(name, &st) < 0) {
    FILE *f = fopen(name, "w");
    if (f) {
      // larger mix buffer to avoid underruns (stutter); 48 kHz is the Switch's native rate
      fprintf(f, "[general]\nfrequency = 48000\nperiod_size = 2048\nperiods = 3\nresampler = linear\n");
      fclose(f);
    }
  }
  setenv("ALSOFT_CONF", name, 1);
}

static void check_data(void) {
  static const char *files[] = {
    GAME_SO_NAME,
    CXX_SO_NAME,
    "x_data.ras",
    "x_english.ras",
    "x_level1.ras",
    "x_level2.ras",
    "x_level3.ras",
    "maxpaynesoundsv2.msf",
    "data",
    "es2",
  };
  struct stat st;
  for (size_t i = 0; i < sizeof(files) / sizeof(*files); ++i) {
    if (stat(files[i], &st) < 0)
      fatal_error("Could not find\n%s\nin the maxpayne folder.\nCheck your data files.", files[i]);
  }
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77))
    fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78))
    fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73))
    fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable.");
  if (appletGetAppletType() != AppletType_Application && appletGetAppletType() != AppletType_SystemApplication)
    fatal_error("Please launch this through a game (hold R while\nstarting a title), not from the album.\nApplet mode does not have enough memory.");
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    // auto; pick resolution based on docked mode
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920;
      screen_height = 1080;
    } else {
      screen_width = 1280;
      screen_height = 720;
    }
  } else {
    screen_width = w;
    screen_height = h;
  }
  debugPrintf("screen mode: %dx%d\n", screen_width, screen_height);
}

static void run_game(void) {
  void *env = jni_env;
  jobject cls = obj_game_native_class;
  jobject empty = jni_new_object_array(0, NULL);

  debugPrintf("JNI_OnLoad\n");
  natives.JNI_OnLoad(jni_vm, NULL);

  debugPrintf("implOnActivityCreated\n");
  natives.OnActivityCreated(env, cls, obj_platform_services, 0);

  debugPrintf("implOnInitialSetup\n");
  natives.OnInitialSetup(env, cls, obj_device_info, obj_asset_manager, empty, empty);

  input_init();

  nwindowSetDimensions(nwindowGetDefault(), screen_width, screen_height);

  debugPrintf("implOnSurfaceCreated\n");
  natives.OnSurfaceCreated(env, cls);

  debugPrintf("implOnSurfaceChanged %dx%d\n", screen_width, screen_height);
  natives.OnSurfaceChanged(env, cls, obj_surface, screen_width, screen_height);

  debugPrintf("entering main loop\n");

  const u64 freq = armGetSystemTickFreq();
  u64 last = armGetSystemTick();
  u64 frames = 0;

  while (appletMainLoop() && !java_quit_requested()) {
    input_update();
    java_process_callbacks();

    const u64 now = armGetSystemTick();
    float dt = (float)(now - last) / (float)freq;
    last = now;
    if (dt > MAX_FRAME_TIME) dt = MAX_FRAME_TIME;

    natives.OnDrawFrame(env, cls, dt);

    ++frames;
    if (config.debug_log)
      gl_debug_frame(frames);
  }

  debugPrintf("exiting\n");
}

static void *game_thread(void *arg) {
  run_game();
  return NULL;
}

int main(int argc, char *argv[]) {
  enter_data_dir(argc, argv);

  // try to read the config file and create one with default values if it's missing
  if (read_config(CONFIG_NAME) < 0)
    write_config(CONFIG_NAME);

  log_init(config.debug_log);
  debugPrintf("maxpayne_nx starting\n");

  check_syscalls();
  check_data();

  set_screen_size(config.screen_width, config.screen_height);
  setup_audio_config();

  debugPrintf("module region = %p, %zu KB\n", heap_so_base, heap_so_limit / 1024);

  // game code reads its stack guard from TPIDR_EL0 on every call
  tls_init_main();

  so_init_region(heap_so_base, heap_so_limit);
  imports_init();

  // savegames and settings end up in here
  mkdir("savegames", 0777);

  so_module *cxx = so_load(CXX_SO_NAME);
  if (!cxx) fatal_error("Could not load\n%s.", CXX_SO_NAME);
  so_module *game = so_load(GAME_SO_NAME);
  if (!game) fatal_error("Could not load\n%s.", GAME_SO_NAME);

  so_relocate(cxx);
  so_relocate(game);

  patch_game(game);

  so_finalize(cxx);
  so_finalize(game);

  patch_game_data(game);

  java_init(game);

  so_run_init(cxx);
  so_run_init(game);

  long thread;
  if (tls_spawn_thread(&thread, game_thread, NULL, GAME_THREAD_STACK, CORE_GAME_RENDER) != 0)
    fatal_error("Could not start the game thread.");
  pthread_join_fake(thread, NULL);

  return 0;
}
