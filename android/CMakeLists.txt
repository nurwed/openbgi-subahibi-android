# This file is part of OpenBGI (https://openbgi.net).
# SPDX-License-Identifier: GPL-2.0-only
#
# android/CMakeLists.txt - the Android build: one shared library, libmain.so,
#                          which SDLActivity loads and starts
#
# SDLActivity.java loads libSDL2.so and then "main" (this library) and calls
# the SDL_main function found in it through nativeRunMain (SDL's own glue is
# in libSDL2.so; the SDL2main library is empty on Android and is not linked).
# src/sys/main.c includes <SDL.h> under __ANDROID__, which is what renames its
# main to SDL_main.
#
#   cmake -S android -B build/android \
#         -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 \
#         -DSDL2_DIR=$HOME/src/SDL -DFREETYPE_DIR=$HOME/src/freetype
#
# android-28 is the oldest level the engine links at: bionic only has
# posix_spawn from there (libc.map: LIBC_P, API 28), which
# src/os/posix/pipeproc.c uses.  iconv is API 28 too, but the Shift-JIS
# conversion does not use it here (src/os/posix/sjis_table.c).
#
# SDL2 and FreeType are built from their own source trees (add_subdirectory),
# so nothing of the host's is used: Vorbis is the single-file decoder
# src/snd/stb_vorbis.c (no libvorbisfile), the sound device is SDL's (no
# ALSA), the faces come from the system font directories (no fontconfig) and
# the window is SDL's (no X11).  The Makefile's own PLATFORM=sdl build stays
# the desktop build; the two share the same sources and flags.
#
# Besides libmain.so the APK needs libSDL2.so (built by the SDL2 tree in the
# same build) in its native library directory.
cmake_minimum_required(VERSION 3.18) # the NDK's android.toolchain.cmake wants 3.18 or newer
project(openbgi C)

# ---- the two source trees ------------------------------------------------------------------

set(SDL2_DIR "" CACHE PATH "SDL2 source tree (the directory with its CMakeLists.txt)")
set(FREETYPE_DIR "" CACHE PATH "FreeType source tree (the directory with its CMakeLists.txt)")

if(NOT SDL2_DIR)
	message(FATAL_ERROR "SDL2_DIR is not set: pass -DSDL2_DIR=<SDL2 source tree>")
endif()
if(NOT FREETYPE_DIR)
	message(FATAL_ERROR "FREETYPE_DIR is not set: pass -DFREETYPE_DIR=<FreeType source tree>")
endif()
get_filename_component(SDL2_DIR "${SDL2_DIR}" ABSOLUTE)
get_filename_component(FREETYPE_DIR "${FREETYPE_DIR}" ABSOLUTE)

get_filename_component(OPENBGI_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# FreeType with every optional dependency off, before its CMakeLists reads the
# options: bionic has no zlib, bzip2, libpng, HarfBuzz or Brotli for it to
# find, and font.c only asks for the TrueType/OpenType outlines.
set(FT_DISABLE_ZLIB     TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   TRUE CACHE BOOL "" FORCE)
add_subdirectory("${FREETYPE_DIR}" "${CMAKE_BINARY_DIR}/freetype")

# SDL2: its Android branch builds libSDL2.so (SDL_SHARED, the default there)
# with the Android video, audio, filesystem, power and sensor drivers, and
# links libdl, liblog and libandroid itself.
add_subdirectory("${SDL2_DIR}" "${CMAKE_BINARY_DIR}/sdl2")

# ---- the engine's sources ------------------------------------------------------------------
#
# The same set the Makefile collects for PLATFORM=sdl: every directory of the
# engine, the SDL back end, and the X11-free files of the POSIX back end (the
# X11 window, dialogs, UI kit and audio device are desktop-only).

set(OPENBGI_DIRS
	src/core src/asm src/scn src/bpc src/dump src/vm src/wait
	src/sys src/sys/install src/sysobj
	src/gfx src/gfx/background src/gfx/mgr src/gfx/particle src/gfx/window src/gfx/text
	src/snd src/snd/bw src/snd/wm
	src/dbg src/dbg/views)

set(OPENBGI_SOURCES)
foreach(dir ${OPENBGI_DIRS})
	file(GLOB _dir_sources "${OPENBGI_ROOT}/${dir}/*.c")
	list(APPEND OPENBGI_SOURCES ${_dir_sources})
endforeach()

# src/snd/*.c already holds stb_vorbis.c; with BGI_USE_STB_VORBIS the BW codec
# 3 includes it header-only (src/snd/bw/bw_internal.h) and this is the one
# translation unit with its implementation.
list(APPEND OPENBGI_SOURCES
	"${OPENBGI_ROOT}/src/os/os_common.c"
	"${OPENBGI_ROOT}/src/os/posix/sjis_table.c")

file(GLOB OPENBGI_SDL_SOURCES "${OPENBGI_ROOT}/src/os/sdl/*.c")
list(APPEND OPENBGI_SOURCES ${OPENBGI_SDL_SOURCES})

foreach(f process paths files time thread machine registry native sjis pipeproc font)
	list(APPEND OPENBGI_SOURCES "${OPENBGI_ROOT}/src/os/posix/${f}.c")
endforeach()

add_library(main SHARED ${OPENBGI_SOURCES})

# ---- the flags the Makefile's PLATFORM=sdl build uses ---------------------------------------

# C99, as CSTD in the Makefile
set_target_properties(main PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

# the Makefile's warnings; the two format ones are GCC's only
set(OPENBGI_WARNINGS -Wall -Wextra -pedantic -Wno-unused-parameter)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
	list(APPEND OPENBGI_WARNINGS -Wno-format-truncation -Wno-format-overflow)
endif()

target_compile_options(main PRIVATE ${OPENBGI_WARNINGS})

# Optimize every configuration of libmain: the configure command above sets
# no CMAKE_BUILD_TYPE, and the NDK's default build type then has no
# optimization (-O0), which misses the frame interval and the sound callback.
# -O2 also overrides the -O3 of a Release build, so every build of libmain is
# the same speed; pass -DCMAKE_C_FLAGS_DEBUG=-O0 for an unoptimized one.
target_compile_options(main PRIVATE -O2)

# BGI_POSIX: the POSIX back end's shared code (os_common.c and the posix
# files).  BGI_USE_STB_VORBIS: the single-file Vorbis decoder.  No
# BGI_HAVE_VORBISFILE, BGI_HAVE_ALSA and BGI_HAVE_FONTCONFIG - those parts of
# the desktop build have no place here (and bionic has no iconv-based CP932:
# sjis.c picks the generated table under __ANDROID__, tools/gen_sjis_table.py).
target_compile_definitions(main PRIVATE BGI_POSIX BGI_USE_STB_VORBIS _POSIX_C_SOURCE=200809L)

target_include_directories(main PRIVATE "${OPENBGI_ROOT}/inc")

# ---- pl_mpeg: the Android movie decoder ------------------------------------------------------
#
# src/sys/plmarc.c is the one translation unit with PL_MPEG_IMPLEMENTATION
# (third_party/pl_mpeg, MIT).  It is built at -O3: arm64 clang auto-vectorizes
# parts of the IDCT, the motion compensation copy and the YUV -> RGB
# conversion.  Everything else stays at the -O2 above.
target_include_directories(main PRIVATE "${OPENBGI_ROOT}/third_party/pl_mpeg")
set_source_files_properties("${OPENBGI_ROOT}/src/sys/plmarc.c" PROPERTIES COMPILE_OPTIONS "-O3")

# ---- link ------------------------------------------------------------------------------------

# SDL2::SDL2 is the in-tree alias of the SDL2 target; older SDL2 trees have
# the plain name only
if(TARGET SDL2::SDL2)
	set(OPENBGI_SDL2 SDL2::SDL2)
else()
	set(OPENBGI_SDL2 SDL2)
endif()

target_link_libraries(main PRIVATE ${OPENBGI_SDL2} freetype log android m)
