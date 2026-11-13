# OpenBGI — Android port

This repository is an Android port of [OpenBGI](https://openbgi.net), the
GPL-2.0 re-implementation of Ethornell's BGI visual-novel engine.  The port
adds an SDL2 back end (`src/os/sdl`) and an Android build
(`android/CMakeLists.txt`), and keeps the existing X11/FreeType/ALSA
desktop back end working.

**No game data is included.**  You must own a copy of the game and point the
engine at your own files.  Do not redistribute the game archives, images,
sounds, movies or extracted scripts.

## Supported game

The port is developed against **Subarashiki Hibi (English release)**, which
uses engine **1.643.1** (`Compatibility : 1.72`).  OpenBGI has no exact
profile for that build; the **1.653** profile is used.  The engine detects
the game from the strings inside `ipl._bp` in `system.arc`, not from the
executable name.

A successful start prints:

```
bgi: engine profile 1.653 selected for this game
```

Other BGI games can be described in `games.json`, which maps a product
string to an engine profile.

## Choosing the game folder

The game folder must contain `system.arc` (and usually `ipl._bp`).

* **Desktop:** run `bin/bgi-sdl <game-folder>`, or run the binary from
  inside the game folder.
* **Android:** the activity passes the folder the player selected as its
  first argument.  The engine reads it as `BGI_GAME_DIR`; its writable
  data directory is `BGI_FILES_DIR`.  The SDL launcher window is not
  implemented on Android, so the folder must be supplied by the app.
* `games.json` is read from the game folder, or from `BGI_FILES_DIR` on
  Android when the game folder has none.

## Controls

| Input | Action |
|---|---|
| Tap | Left click |
| Long press (500 ms) | Right click |
| Two-finger tap | Right click |
| Mostly vertical swipe | Mouse wheel (one notch per 48 picture pixels) |
| Back | Menu |
| Corner button (top-right) | Holds the in-game button row (Skip/Auto/Save/Load/Log) up; tap again to release it |

The engine polls the pointer once per frame, so a tap is delivered as a
press 40 ms after the finger and a release 80 ms later.

## Building

### Desktop (Linux, SDL2)

Requirements: a C99 compiler, GNU make, SDL2, FreeType and libvorbisfile
development files.

```sh
make PLATFORM=sdl
bin/bgi-sdl <game-folder>
```

### Android

The Android build itself is CMake-only.  It produces `libmain.so`, which
an Android activity loads together with `libSDL2.so`.  The build expects
SDL2 and FreeType source trees and the engine sources.  If you package it
with an Android Studio/Gradle wrapper, that wrapper can read them from
`local.properties`:

```properties
# Android SDK / NDK
sdk.dir=/path/to/Android/Sdk
ndk.dir=/path/to/Android/Sdk/ndk/<ndk-version>
cmake.dir=/path/to/Android/Sdk/cmake/<cmake-version>

# source trees used by android/CMakeLists.txt
sdl2.dir=/path/to/SDL2
freetype.dir=/path/to/freetype
engine.dir=/path/to/openbgi-sdl
```

Use an NDK that supports API level 28 (`posix_spawn` is needed by the
engine) and CMake 3.18 or newer.  Set the Gradle `ndkVersion` and
`cmake.version` to the installed versions.

The equivalent direct CMake invocation is:

```sh
cmake -S android -B build/android \
      -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a \
      -DANDROID_PLATFORM=android-28 \
      -DSDL2_DIR=$SDL2 \
      -DFREETYPE_DIR=$FREETYPE
cmake --build build/android
```

The Android build also needs the single-file Vorbis decoder
`src/snd/stb_vorbis.c` (public domain, from
[nothings/stb](https://github.com/nothings/stb)).  It is present in the
working tree but is currently listed in `.gitignore`; fetch or add it
before building.

The APK must not contain game data.  The player selects their own game
folder.

## Known issues

* Dialogue letter spacing is uneven when the requested font (Consolas) is
  missing.  The original engine behaves the same way; `91 0D`
  SetPropFontAdvance is not used by the glyph renderer, and this game never
  calls it.
* CompressedBG v2 alpha mode 2 (a Huffman-coded alpha plane) is not
  implemented in `Cbg2_Decode`.  Pictures that use it are drawn fully
  opaque.

## License

OpenBGI is licensed under the GNU General Public License, version 2.0
only.  See `LICENSE`.

Bundled third-party components and the licenses of the libraries linked
into the Android build are listed in `THIRD_PARTY.md`.
