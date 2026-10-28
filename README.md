# maxpayne2_nx

A loader that runs the Android version of **Max Payne Mobile 2.1.131** natively on a
modded Nintendo Switch.

It loads the game's own ARM64 `libGame.so` into a Switch homebrew process, provides
the parts of Android (bionic libc, the NDK, and the Java side of the app) that the game
expects, and patches a handful of functions. No emulation is involved: the game code
runs directly on the Switch CPU, and rendering goes through Mesa's OpenGL ES driver.

This project is a heavily reworked descendant of
[max_nx](https://github.com/fgsfdsfgs/max_nx) by fgsfds, which runs **version 1.7**.
Version 2.x changed the game's whole Android layer, so max_nx cannot load it; see
[Relationship to max_nx](#relationship-to-max_nx) for what was kept and what had to
be rewritten.

**No game files are included.** You need your own copy of the game.

## Status

Playable: the game boots, menus, the opening graphic novel and gameplay render, and
audio and controls work. Tested on a Switch Lite (Atmosphère, firmware 22.1).

Not supported:

- intro / credits videos (skipped)
- Rockstar Social Club, cloud saves and other online features (disabled)
- on-screen keyboard, vibration
- switching bloom off (see [Notes](#notes))

## Requirements

- A Switch running Atmosphère with the homebrew menu.
- Max Payne Mobile **2.1.131** for Android (arm64-v8a). Other 2.x versions may work
  if the exported function names are unchanged; 1.x versions do not (use max_nx).
- About 1.5 GB free on the SD card (about 600 MB for English only).

## Installation

1. Download `maxpayne.nro` from the Releases page, or build it (see [Building](#building)).
2. Create `/switch/maxpayne/` on the SD card and put these in it:
   - `maxpayne.nro`
   - `libGame.so` from the APK's `lib/arm64-v8a/` folder
   - `libc++_shared.so` from the Android NDK r29:
     `toolchains/llvm/prebuilt/<host>/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so`
     (the copy in the APK is byte-identical in its code and works too)
3. Copy **the contents** of the APK's `assets/` folder into the same folder, so that
   `/switch/maxpayne/x_data.ras` and `/switch/maxpayne/es2/` exist. `flutter_assets/`,
   `dexopt/` and `rockstar/` aren't needed. For English only you need:
   `x_data.ras`, `x_english.ras`, `x_level1.ras`, `x_level2.ras`, `x_level3.ras`,
   `maxpaynesoundsv2.msf`, `es2/`, `data/`.

   Version 2.x ships its data as Play Asset Delivery packs instead of an OBB. If your
   APK's `assets/` folder has no `.ras`/`.msf` files, take them from the installed
   asset packs on your phone.

```
/switch/maxpayne/
  maxpayne.nro  libGame.so  libc++_shared.so
  x_data.ras  x_english.ras  x_level1.ras  x_level2.ras  x_level3.ras
  maxpaynesoundsv2.msf   es2/   data/
```

## Running

The game needs the full application memory pool, so it must replace a game rather
than run from the album:

1. On the HOME menu, **hold R** while starting any installed game.
2. In the homebrew menu, choose **Max Payne Mobile**.

The first launch creates `config.txt`, `alsoft.conf`, `debug.log` and `savegames/`
in `/switch/maxpayne/`.

## Controls

| Switch | Game |
|---|---|
| Left stick / right stick | move / look |
| ZR / ZL | right / left trigger |
| R / L | right / left bumper |
| B, A, Y, X | bottom, right, left, top face button (Xbox positions) |
| + | Start (pause) |
| − | Back |
| D-pad | D-pad |
| L3 / R3 | stick clicks |
| Touch screen | touch input |

## Configuration

`config.txt`:

| Option | Default | Meaning |
|---|---|---|
| `screen_width` / `screen_height` | -1 | render size; -1 = auto (1280×720 handheld, 1920×1080 docked) |
| `trilinear_filter` | 1 | force trilinear texture filtering |
| `disable_mipmaps` | 0 | skip mip levels of compressed textures |
| `language` | 0 | default language (0 = English, 2 = German); can also be changed in game |
| `crouch_toggle` | 1 | crouch button toggles instead of being held |
| `character_shadows` | 1 | 1 = blob shadow, 2 = foot shadows |
| `drop_highest_lod` | 0 | lower model detail |
| `show_weapon_menu` | 0 | show the touch weapon-swipe arrows |
| `decal_limit` / `debris_limit` | 0.5 / 1.0 | effect limits |
| `touch_screen` | 0 | report a touch screen to the game (shows touch controls) |
| `debug_log` | 1 | write `debug.log` |
| `gl_trace` | 0 | check every GL call for errors and log one full frame (slow; debugging only) |

`alsoft.conf` holds OpenAL Soft settings (sample rate, buffer size, resampler). Raise
`period_size` if audio crackles.

## Troubleshooting

- An error screen names the missing file or the problem.
- If the game crashes, `/switch/maxpayne/crash.log` contains the registers and a
  backtrace with addresses translated to `libGame.so+offset (symbol)` or
  `maxpayne.nro+offset`. Attach it together with `debug.log` when reporting a problem.
- "Please launch this through a game": start it by holding R on a game, not from
  the album.

## Building

Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the Switch
toolchain, then:

```
dkp-pacman -S switch-dev switch-mesa switch-libdrm_nouveau switch-openal-soft switch-mpg123
make
```

(`switch-sdl2` is pulled in by `switch-openal-soft`; on Windows, run these from the
devkitPro MSYS2 shell.) The output is `maxpayne.nro`.

`source/gl_trace.c` is generated by `tools/gen_gl_trace.py` from Mesa's `GLES2/gl2.h`
and the GL entries in `source/imports.c`. Regenerate it if you change which GL
functions are imported:

```
python tools/gen_gl_trace.py $DEVKITPRO/portlibs/switch/include/GLES2/gl2.h source/imports.c source/gl_trace.c
```

## How it works

1. **Loading.** `main.c` splits the heap: newlib gets most of it, and a 64 MB region
   at the top holds the modules. `so_util.c` loads `libc++_shared.so` and `libGame.so`,
   applies their relocations, resolves imports, patches functions, then maps each
   module as code (`svcMapProcessCodeMemory`, needs CFW) with RX/RW page permissions
   and runs its initializers.
2. **Android C library.** `bionic_*.c` and `imports.c` provide the 352 + 163 symbols
   the two modules import, translating where bionic and newlib differ (struct layouts,
   constants, thread-local storage).
3. **Java.** The game talks to Java through JNI. `fake_jni.c` implements the JNIEnv and
   JavaVM function tables; `java_classes.c` implements the few Java classes the game
   calls (`GamePlatformServices`, `NvUtil`, `DeviceInfo`) and answers asynchronous
   requests (Rockstar sign-in gate, HTTP, music playlists) the way the Android app does.
4. **Lifecycle.** `main.c` drives the game the way the app's Kotlin `GameThread` does:
   `JNI_OnLoad` → `implOnActivityCreated` → `implOnInitialSetup` →
   `implOnSurfaceChanged` (the game sets up EGL itself on the default `NWindow`) →
   `implOnDrawFrame(dt)` every frame, on a dedicated thread with a 16 MB stack.
5. **Input.** `input.c` sends Switch controller and touch input through the game's own
   `implOnGamepad*` / `implOnTouch*` callbacks, using the button numbering decoded from
   the app's `InputHandler` class.

## Relationship to max_nx

max_nx was the starting point and is where the method comes from: load the Android
`.so` into a homebrew process, map it as code with the CFW-only memory syscalls,
replace Android functions by name and hook game functions by overwriting their first
instructions. That approach carried over to 2.x unchanged, along with several pieces
of code.

Most of max_nx's version-specific code did not carry over, because 2.x rebuilt the
Android side of the game:

- 1.7 used NVIDIA's `NVEvent` framework, where the native code owns the main loop.
  2.x uses a Rockstar Kotlin layer (`oswrapper`) that calls into native code once per
  frame.
- 1.7 linked the C++ runtime and OpenAL statically; 2.x links them dynamically.
- 2.x is built with a much newer NDK (r29), which uses different relocation and
  thread-local-storage formats.

Of the 61 game symbols max_nx references (besides OpenAL), 31 no longer exist in 2.1.131.

### Kept from max_nx (adapted)

| Area | Files |
|---|---|
| Overall method; CFW syscall checks; heap split between newlib and modules | `main.c` |
| `hook_arm64` patch (`LDR X17, #8; BR X17`) | `so_util.c` (`so_hook`) |
| Config file parser and most options | `config.c`, `config.h` |
| Error screen | `error.c` |
| Logging, `ret0`/`ret1` helpers | `util.c`, `util.h` |
| Detail-level, weapon-swiper and crouch-toggle hooks | `hooks.c` |
| Lazily created pthread objects behind bionic's structs; mipmap and trilinear GL hooks | `bionic_pthread.c`, `imports.c` |
| Makefile | `Makefile` |

### Rewritten or new

| Area | What changed and why | Files |
|---|---|---|
| Loader | max_nx loads one module with exactly one code and one data segment. 2.x needs two modules (`libGame.so` plus `libc++_shared.so`), several writable segments (max_nx would drop `.data.rel.ro`/`.got`/`.init_array`), packed RELR relocations, cross-module symbol lookup, symbol hashing for 37,000 exports, and size-checked patching (some targets are only 8 bytes, too short for a 16-byte jump). | `so_util.c` |
| C++ exceptions | 1.7 had a static C++ runtime and max_nx redirected `__cxa_throw` to newlib's. 2.x uses `libc++_shared.so`, whose unwinder needs `dl_iterate_phdr` to find each module's unwind tables. | `so_util.c`, `bionic_libc.c` |
| Thread-local storage | The game reads its stack-protector value from the TPIDR_EL0 register in 3,691 places. max_nx set that up for the main thread only; here every thread gets an Android-style TLS block. The same block also holds pthread keys and emulated `thread_local` storage (`__emutls_get_address`). | `bionic_tls.c` |
| Threads | Android-layout thread attributes, join and detach handles, thread-exit destructors. Game threads are pinned to CPU cores 1–2 so audio keeps core 0 (fixes audio stutter). | `bionic_tls.c` |
| Synchronization | Android's lock structs are only 4-byte aligned, and 64-bit atomics fault on 4-byte-aligned addresses, so pointers are stored at the first 8-byte-aligned offset inside each struct. Added rwlocks, semaphores, a race-free `pthread_once`, and correct recursive mutexes (max_nx's recursive-mutex attribute was never applied). | `bionic_pthread.c` |
| libc compatibility | Android's stdio layout (`__sF` with 152-byte FILEs), `stat`/`dirent` structs, Linux clock IDs and open flags, `sysconf` names, fortify `*_chk` functions, the `futex`/`gettid` syscalls, and stubs for the locale functions libc++ uses. | `bionic_libc.c`, `imports.c` |
| Android NDK | `AAssetManager`/`AAsset` reads from the SD card; `ANativeWindow` maps to libnx's default `NWindow`. | `android_ndk.c` |
| JNI | max_nx deliberately had no JNI and crashed on any JNI call. 2.x passes Java objects into native code, so this is a full JNIEnv/JavaVM implementation with strings, arrays and method and field dispatch. | `fake_jni.c`, `fake_jni.h` |
| Java side | Behaviour of `GamePlatformServices`, `NvUtil` and `DeviceInfo`, a queue for asynchronous callbacks, and the native entry points. | `java_classes.c` |
| Startup | `NVEventAppMain` is gone; the app's lifecycle calls are reproduced instead. EGL setup moved into the game, and `ANativeWindow` is all it needs. | `main.c` |
| Input | Polled `WarGamepad_*` hooks replaced by pushing events through `implOnGamepad*`/`implOnTouch*`. Button numbering was decoded from the app's `InputHandler.toGamepadButton` and axis order from `onGamepadAxesChanged`. | `input.c` |
| Audio | OpenAL is now an import and goes straight to openal-soft (max_nx needed 130 hooks for the statically linked copy); mpg123 likewise. `alsoft.conf` sets the buffer size. | `imports.c`, `main.c` |
| GL compatibility | Mesa rejects uniform calls whose type doesn't match the shader declaration, which mobile drivers accept; these are retried with the correct call. | `gl_uniforms.c` |
| Diagnostics | Crash handler with symbolized registers and backtrace; optional GL error tracing (one wrapper per GL function, generated from `gl2.h`) and a one-frame call trace. | `error.c`, `gl_debug.c`, `gl_trace.c`, `tools/gen_gl_trace.py` |

### Removed

| max_nx feature | Reason |
|---|---|
| `use_bloom` option (forcing `UseBloom()`) | In 2.x, `UseBloom()` always returns 1, and the bloom pipeline also draws the final image. Forcing it to 0 skipped creating the bloom shaders, so the 3D view rendered black. |
| `mod_file` option | The function it hooked (`R_File::setFileSystemRoot`) is no longer exported. |
| OpenAL hooks, `NVEvent*`/`WarGamepad_*`/`AND_*` hooks | The functions no longer exist, or are now imports. |

## Notes

Behaviours of 2.x that this loader has to reproduce, for anyone working on similar
ports:

- **Startup state machine.** `ApplicationTick` plays the credits movie, then the intro
  movie, then waits for `isSplashScreenVisible()` to become **true** before counting
  three seconds and moving on. A stub that always returns false hangs startup with a
  black screen.
- **Rockstar gate.** The game proceeds once `implOnRockstarInitialComplete` has been
  called in response to `rockstarShowInitial()`. `OS_IsRockstarCloudEnabled` is patched
  to return 0 so the Social Club HTTP/cloud code never starts.
- **Bloom.** See *Removed* above.
- **GLES context.** Mesa returns an ES 3.2 context even though the game requests ES 2.

## Credits

- fgsfds and TheOfficialFloW: [max_nx](https://github.com/fgsfdsfgs/max_nx),
  [max_vita](https://github.com/fgsfdsfgs/max_vita) and the .so-loading method this
  project builds on.
- Switchbrew (libnx) and the devkitPro team (toolchain, Mesa, OpenAL Soft, mpg123
  and SDL2 ports).

## Legal

This project has no affiliation with Take-Two Interactive Software, Rockstar Games
or Remedy Entertainment. "Max Payne" is a trademark of its respective owners.

The repository contains no game code or assets. You must provide them from a copy
of the game you own. Do not open issues asking for game files.

The source code is available under the MIT license; see [LICENSE](LICENSE).
