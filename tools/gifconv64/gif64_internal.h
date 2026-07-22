/**
 * @file gif64_internal.h
 * @brief On-disk format of a GIF64 predecoded animation stream
 *
 * A GIF64 file is an animated GIF that has been fully decoded offline into a
 * stream of CI8 (8-bit palette-indexed) frames, so that the N64 can play it
 * back with ZERO on-device LZW decoding: each frame is a raw palette-index
 * plane plus a per-frame 256-entry RGBA5551 TLUT that the RDP can load and
 * blit directly.
 *
 * All multi-byte fields are BIG-ENDIAN (the N64 native byte order), so the
 * file can be streamed straight into RDRAM with no byteswap.
 *
 * File layout:
 * @code
 *   gif64_header_t                          // 16 bytes
 *   uint16_t delay_ms[nframes]              // per-frame GCE delay, big-endian
 *   // then, repeated nframes times:
 *   uint8_t  index[width*height]            // CI8 palette-index plane
 *   uint16_t tlut[256]                      // RGBA5551 palette, big-endian
 * @endcode
 *
 * Each frame is composited (GIF disposal + transparency resolved) and scaled
 * to width x height before quantization, so the stream is a self-contained
 * flip-book: no inter-frame state is needed to display frame N.
 */
#ifndef __LIBDRAGON_GIF64_INTERNAL_H
#define __LIBDRAGON_GIF64_INTERNAL_H

#include <stdint.h>

#define GIF64_ID           "GC64"   ///< GIF64 file identifier ("Gif → CI8, N64")
#define GIF64_VERSION      1         ///< Current file format version

#define GIF64_FLAG_LOSSY   (1 << 0)  ///< At least one frame was palette-reduced (median-cut)

#define GIF64_TLUT_COLORS  256       ///< Palette entries per frame (CI8)
#define GIF64_TLUT_BYTES   (GIF64_TLUT_COLORS * 2)  ///< TLUT size in bytes (RGBA5551)

/** @brief Header of a GIF64 file (all fields big-endian). */
typedef struct __attribute__((packed)) {
	char     id[4];       ///< ID of the file (#GIF64_ID)
	uint16_t version;     ///< Version of the file (#GIF64_VERSION)
	uint16_t flags;       ///< Misc flags (#GIF64_FLAG_LOSSY)
	uint32_t nframes;     ///< Number of frames in the animation
	uint16_t width;       ///< Frame width in pixels
	uint16_t height;      ///< Frame height in pixels
} gif64_header_t;

_Static_assert(sizeof(gif64_header_t) == 16, "invalid gif64_header size");

#endif
