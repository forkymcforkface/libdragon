# Makefile changes to register gifconv64

These are the exact edits to `libdragon/tools/Makefile` needed to build
`gifconv64`. They are documented here rather than applied so the submodule's
tracked `Makefile` stays clean; apply them verbatim when opening the PR. The
pattern mirrors `audioconv64` / `mksprite` exactly.

`gifconv64` is a single translation unit: `gifconv64.cpp` textually includes
`conv_gif64.cpp` (which includes the `stb_image.h` implementation and the
vendored `binout.c`), so only one object file is produced.

## 1. Add the object list

Add this line next to the other `*_OBJS` definitions (e.g. just before the
`videoconv64_OBJS` / `audioconv64_OBJS` block, around line 119):

```make
gifconv64_OBJS = gifconv64/gifconv64.o
```

## 2. Add the per-object CXXFLAGS

The vendored `stb_image.h` defines two static helpers that are unused when built
with `STBI_ONLY_GIF`, which trips `-Wunused-function` under the Makefile's
`-Werror`. Silence just that warning for this one object (this mirrors the
per-object CFLAGS lines already present for `mksprite`'s vendored `x264` and
`audioconv64`'s `libvadpcm`, around lines 80-88):

```make
gifconv64/gifconv64.o: CXXFLAGS += -Wno-unused-function
```

## 3. Register the tool

Append `gifconv64` to the `TOOLS` list (line 132):

```make
TOOLS = n64tool n64metadata n64sym n64symdump n64elfcompress ed64romconfig audioconv64 gifconv64 videoconv64 mkdfs dumpdfs mkasset mksprite mkfont mkmodel mkmaterial n64dso n64dso-msym n64dso-extern rdpvalidate combexpr cpaktool
```

(Only the addition of ` gifconv64` matters; placement in the list is cosmetic.)

## Unified diff

```diff
@@ tools/Makefile @@
+gifconv64/gifconv64.o:             CXXFLAGS += -Wno-unused-function
 audioconv64/libvadpcm.o:           CFLAGS += -Iaudioconv64/vadpcm
@@ tools/Makefile @@
 audioconv64_OBJS = audioconv64/audioconv64.o audioconv64/libvadpcm.o audioconv64/libopus.o audioconv64/libsamplerate.o audioconv64/liblzh5.o audioconv64/libxm.o common/assetcomp.a
+gifconv64_OBJS = gifconv64/gifconv64.o
 videoconv64_OBJS = videoconv64/videoconv64.o ...
@@ tools/Makefile @@
-TOOLS = n64tool n64metadata n64sym n64symdump n64elfcompress ed64romconfig audioconv64 videoconv64 mkdfs dumpdfs mkasset mksprite mkfont mkmodel mkmaterial n64dso n64dso-msym n64dso-extern rdpvalidate combexpr cpaktool
+TOOLS = n64tool n64metadata n64sym n64symdump n64elfcompress ed64romconfig audioconv64 gifconv64 videoconv64 mkdfs dumpdfs mkasset mksprite mkfont mkmodel mkmaterial n64dso n64dso-msym n64dso-extern rdpvalidate combexpr cpaktool
```

The existing `TOOL_template` `$(foreach ...)` at the bottom of the Makefile then
generates the `gifconv64`, `gifconv64-install`, and `gifconv64-clean` targets
automatically — no further changes are required.

## Verified

Built with MinGW-w64 (the same cross-toolchain path used to build
`audioconv64.exe`) using the Makefile's own `-O2 -pthread -Wall -Werror` flags
plus the per-object rule above:

```
    [CXX] gifconv64/gifconv64.o
    [TOOL] gifconv64
```

produced a static `gifconv64.exe` that converts real theme GIFs (verified: an
already-320x240 GIF round-trips fully lossless; scaled/reduced-palette GIFs use
the median-cut fallback and set `GIF64_FLAG_LOSSY`).
