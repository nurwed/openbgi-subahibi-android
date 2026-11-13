# Third-party components

OpenBGI itself is licensed under the GNU General Public License, version
2.0 only.  The full text is in `LICENSE`.

## Included in this repository

| Component | License | Location |
|---|---|---|
| OpenBGI engine (including the SDL and POSIX back ends) | GPL-2.0-only | `src/`, `inc/`, `android/`; `LICENSE` |
| OpenBGI tools source snapshot | GPL-2.0-only | `tools/`; `tools/LICENSE` |
| pl_mpeg — MPEG-1 video/MP2 audio decoder and MPEG-PS demuxer | MIT | `third_party/pl_mpeg/pl_mpeg.h`, `third_party/pl_mpeg/LICENSE` |
| stb_vorbis — Ogg Vorbis decoder | Public domain / MIT | `src/snd/stb_vorbis.c` (single-file decoder) |

`third_party/pl_mpeg/LICENSE` contains the MIT text for the exact
`pl_mpeg.h` revision used here.  `src/snd/stb_vorbis.c` contains its own
public-domain/MIT notice at the end of the file.

## Built from external source trees

The Android build compiles SDL2 and FreeType from source trees supplied by
the builder (see `local.properties` in `README.md`).  Their binaries are
part of the APK, so their license notices must accompany the APK:

| Component | License | What to ship |
|---|---|---|
| SDL2 | zlib license | SDL2's `LICENSE.txt` (zlib text) with the APK notices |
| FreeType | FreeType License (FTL) or GPLv2 | FreeType's `LICENSE.TXT` / `FTL.TXT`, or the GPLv2 text if built under GPLv2 |

FreeType is normally used under the FTL, which is compatible with this
GPL-2.0 project.  If it is built under GPLv2 instead, include the GPLv2
text as well.

The Android NDK, the Android platform libraries (`libc`, `libm`, `liblog`,
`libandroid`) and the C library are part of the Android runtime and are
not redistributed by this repository.

## Runtime dependencies on the desktop

The SDL desktop build links SDL2 and FreeType from the system and uses
libvorbisfile when available; the X11 build additionally links X11, Xext
and ALSA.  These are system libraries and are not bundled in this
repository.
