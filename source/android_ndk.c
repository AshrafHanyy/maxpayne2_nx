/* android_ndk.c -- libandroid / liblog replacements
 *
 * AAssetManager reads from the data directory (the APK's assets/ folder
 * copied to the SD card). ANativeWindow is the default libnx NWindow,
 * which Mesa's EGL accepts as a native window.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "config.h"
#include "util.h"

#define ASSET_BUFFER_SIZE (64 * 1024)

typedef struct {
  FILE *f;
  int64_t length;
} fake_asset;

void *AAssetManager_fromJava_fake(void *env, void *obj) {
  return (void *)0x1000; // never dereferenced by the game
}

void *AAssetManager_open_fake(void *mgr, const char *name, int mode) {
  // misses are normal: the game probes for loose files before using the .ras archives
  FILE *f = fopen(name, "rb");
  if (!f) return NULL;
  setvbuf(f, NULL, _IOFBF, ASSET_BUFFER_SIZE);
  fake_asset *a = calloc(1, sizeof(*a));
  a->f = f;
  fseeko(f, 0, SEEK_END);
  a->length = ftello(f);
  fseeko(f, 0, SEEK_SET);
  return a;
}

int AAsset_read_fake(void *asset, void *buf, size_t count) {
  fake_asset *a = asset;
  const size_t r = fread(buf, 1, count, a->f);
  if (r == 0 && ferror(a->f)) return -1;
  return (int)r;
}

int64_t AAsset_seek64_fake(void *asset, int64_t offset, int whence) {
  fake_asset *a = asset;
  if (fseeko(a->f, offset, whence) != 0) return -1;
  return ftello(a->f);
}

int64_t AAsset_getLength64_fake(void *asset) {
  return ((fake_asset *)asset)->length;
}

int64_t AAsset_getRemainingLength64_fake(void *asset) {
  fake_asset *a = asset;
  return a->length - ftello(a->f);
}

void AAsset_close_fake(void *asset) {
  fake_asset *a = asset;
  if (!a) return;
  fclose(a->f);
  free(a);
}

void *ANativeWindow_fromSurface_fake(void *env, void *surface) {
  return nwindowGetDefault();
}

int ANativeWindow_getWidth_fake(void *win) { return screen_width; }
int ANativeWindow_getHeight_fake(void *win) { return screen_height; }

int ANativeWindow_setBuffersGeometry_fake(void *win, int w, int h, int format) {
  nwindowSetDimensions((NWindow *)win, screen_width, screen_height);
  return 0;
}

void ANativeWindow_release_fake(void *win) { }

int __android_log_print_fake(int prio, const char *tag, const char *fmt, ...) {
  va_list list;
  debugPrintf("[%s] ", tag ? tag : "");
  va_start(list, fmt);
  debugVPrintf(fmt, list);
  va_end(list);
  debugPrintf("\n");
  return 0;
}
