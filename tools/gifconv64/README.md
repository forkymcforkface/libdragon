# gifconv64

`gifconv64` converts an animated **GIF** into a **GIF64** file: the GIF fully
decoded, offline, into a stream of **CI8** frames the N64 can play back with
**zero on-device LZW decoding**.

Decoding an animated GIF on the N64 at runtime is expensive: the LZW decode of a
full-screen GIF runs at only a handful of frames per second. `gifconv64` moves
that work to the PC. For every frame it emits an 8-bit palette-index plane plus a
256-entry RGBA5551 TLUT, so at runtime the console just DMAs each frame off
storage and blits it (CI8 + TLUT) — the GIF plays at its native rate.

## Usage

```
gifconv64 [flags] <file-or-dir> [[flags] <file-or-dir>..]
```

A `.gif` input becomes a `.gif64` output. Like the other libdragon asset tools,
you can pass individual files or whole directories; directory trees are mirrored
into the output directory.

### Options

| Flag | Description |
|------|-------------|
| `-o` / `--output <dir>` | Output directory (default: current directory) |
| `-v` / `--verbose` | Verbose output |
| `-h` / `--help` | Show help |
| `-s` / `--size <WxH>` | Target frame size (default: `320x240`) |
| `-c` / `--colors <N>` | Palette cap per frame, `2..256` (default: `256`) |

### Examples

```sh
# Convert a single GIF to a 320x240 GIF64 in the current directory
gifconv64 background.gif

# Convert every GIF under assets/ into filesystem/, scaled to 256x224
gifconv64 -v -s 256x224 -o filesystem/ assets/
```

## CI8 fidelity

A single GIF frame is, by the format's own definition, a palette image with **at
most 256 colors**, so a CI8 index plane + 256-entry TLUT is the natural fit — half
the bytes of a 16-bit RGBA5551 frame, and for a simple GIF it reproduces the frame
**exactly** rather than reducing quality.

Animated GIFs are trickier. `gifconv64` stores each frame already **composited**
over the canvas (so playback needs no inter-frame state), and a composited frame
can merge colors from several frames' local palettes — it may hold **more than 256
distinct colors even at the source's native size**. A frame also gains colors when
it is **scaled** (bilinear resampling interpolates new ones), or is capped when
`--colors` is set below its real color count. Whenever a frame ends up with more
colors than the palette cap, it is reduced with a pixel-weighted **median cut** and
the file's `GIF64_FLAG_LOSSY` flag is set; frames that fit within the cap stay
lossless. `-v` reports how many frames were quantized.

## File format

See [`gif64_internal.h`](gif64_internal.h) for the authoritative definition.
All multi-byte fields are **big-endian** (N64 native), so the file streams into
RDRAM with no byteswap.

```
gif64_header_t (16 bytes):
    char     id[4]      // "GC64"
    uint16_t version    // 1
    uint16_t flags      // bit0 = GIF64_FLAG_LOSSY
    uint32_t nframes
    uint16_t width
    uint16_t height
uint16_t delay_ms[nframes]        // per-frame GIF GCE delay, in milliseconds

// then, repeated nframes times:
    uint8_t  index[width*height]  // CI8 palette-index plane
    uint16_t tlut[256]            // RGBA5551 palette (alpha bit = GIF transparency)
```

Each frame is self-contained: GIF disposal is composited during conversion, and
per-pixel transparency is preserved in the TLUT entry's RGBA5551 alpha bit — so
displaying frame *N* needs no other frame.

## Dependencies

GIF decoding — including disposal-method compositing, transparency, and per-frame
delays — is handled by the vendored public-domain single-header
[`stb_image.h`](stb_image.h) (`stbi_load_gif_from_memory`), matching how
`audioconv64` vendors `dr_wav.h` / `dr_mp3.h`. Everything else (scaling,
quantization, big-endian output) is self-contained in
[`conv_gif64.cpp`](conv_gif64.cpp).

## Building

`gifconv64` builds with the rest of the libdragon host tools (`make` in
`tools/`). It needs no libdragon target toolchain — it is a pure host tool like
`mksprite` and `audioconv64`.
