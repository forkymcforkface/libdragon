/*
    conv_gif64: convert animated GIF files to GIF64 CI8 streams

    This tool is part of the Libdragon SDK.

    This is free and unencumbered software released into the public domain.

    For more information, please refer to <http://unlicense.org/>
*/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <string.h>
#include <stdlib.h>
#include <vector>
#include <algorithm>
#include "gif64_internal.h"
#include "../common/binout.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_GIF               // we only ever decode GIFs here
#include "stb_image.h"

int flag_gif_width = 320;
int flag_gif_height = 240;
int flag_gif_colors = 256;

// A composited, scaled RGBA frame together with its GCE delay (in milliseconds).
typedef struct {
	std::vector<uint8_t> rgba;      // width*height*4, top-to-bottom, R,G,B,A
	int delay_ms;                   // GIF Graphic Control Extension delay
} gif_frame_t;

// Encode an 8-bit RGBA quad into RGBA5551. The GIF's per-pixel transparency maps
// to the single alpha bit (opaque when a>=128, transparent otherwise). This is
// the exact packing the N64 RDP expects for a 16-bit TLUT entry.
static inline uint16_t conv_rgba5551(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	return (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >= 128 ? 1 : 0));
}

// Decode a whole animated GIF into composited RGBA frames. stb_image resolves
// GIF disposal methods and transparency for us, returning every frame already
// composited over the canvas, plus each frame's delay (already in ms).
static bool read_gif(const char *infn, std::vector<gif_frame_t> *out, int *gw, int *gh)
{
	FILE *f = fopen(infn, "rb");
	if (!f) {
		fprintf(stderr, "ERROR: %s: cannot open file\n", infn);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0) {
		fprintf(stderr, "ERROR: %s: empty file\n", infn);
		fclose(f);
		return false;
	}
	uint8_t *buf = (uint8_t*)malloc(len);
	if (!buf) {
		fprintf(stderr, "ERROR: %s: out of memory reading file\n", infn);
		fclose(f);
		return false;
	}
	if (fread(buf, 1, len, f) != (size_t)len) {
		fprintf(stderr, "ERROR: %s: cannot read file\n", infn);
		free(buf);
		fclose(f);
		return false;
	}
	fclose(f);

	int x, y, z, comp;
	int *delays = NULL;
	// req_comp=4: force RGBA so every frame has the same 4-byte layout.
	uint8_t *frames = stbi_load_gif_from_memory(buf, (int)len, &delays, &x, &y, &z, &comp, 4);
	free(buf);
	if (!frames) {
		fprintf(stderr, "ERROR: %s: not a valid GIF file (%s)\n", infn, stbi_failure_reason());
		return false;
	}

	*gw = x; *gh = y;
	size_t framesz = (size_t)x * y * 4;
	for (int i = 0; i < z; i++) {
		gif_frame_t fr;
		fr.rgba.assign(frames + (size_t)i * framesz, frames + (size_t)(i + 1) * framesz);
		fr.delay_ms = delays ? delays[i] : 0;
		out->push_back(std::move(fr));
	}
	stbi_image_free(frames);
	free(delays);
	return true;
}

// Bilinearly resample an RGBA frame to (dw x dh). Returns a freshly allocated
// buffer of dw*dh*4 bytes. Frames already at the target size are copied as-is,
// which keeps them lossless (no interpolated colors are introduced).
static std::vector<uint8_t> resize_rgba(const std::vector<uint8_t> &src, int sw, int sh, int dw, int dh)
{
	if (sw == dw && sh == dh)
		return src;

	std::vector<uint8_t> dst((size_t)dw * dh * 4);
	for (int dy = 0; dy < dh; dy++) {
		float fy = (dy + 0.5f) * sh / dh - 0.5f;
		int y0 = (int)floorf(fy);
		float wy = fy - y0;
		int y1 = y0 + 1;
		if (y0 < 0) y0 = 0;
		if (y1 > sh - 1) y1 = sh - 1;
		if (y0 > sh - 1) y0 = sh - 1;
		for (int dx = 0; dx < dw; dx++) {
			float fx = (dx + 0.5f) * sw / dw - 0.5f;
			int x0 = (int)floorf(fx);
			float wx = fx - x0;
			int x1 = x0 + 1;
			if (x0 < 0) x0 = 0;
			if (x1 > sw - 1) x1 = sw - 1;
			if (x0 > sw - 1) x0 = sw - 1;
			const uint8_t *p00 = &src[((size_t)y0 * sw + x0) * 4];
			const uint8_t *p01 = &src[((size_t)y0 * sw + x1) * 4];
			const uint8_t *p10 = &src[((size_t)y1 * sw + x0) * 4];
			const uint8_t *p11 = &src[((size_t)y1 * sw + x1) * 4];
			uint8_t *d = &dst[((size_t)dy * dw + dx) * 4];
			for (int c = 0; c < 4; c++) {
				float top = p00[c] * (1 - wx) + p01[c] * wx;
				float bot = p10[c] * (1 - wx) + p11[c] * wx;
				d[c] = (uint8_t)(top * (1 - wy) + bot * wy + 0.5f);
			}
		}
	}
	return dst;
}

// ---- Median-cut quantizer (lossy fallback for frames with > flag_gif_colors
//      distinct RGBA5551 colors). Operates on the histogram of on-device 5551
//      colors, so the resulting palette is expressed exactly in device space. --
typedef struct { uint16_t c5551; uint32_t count; } ucolor_t;

static int g_axis;    // channel currently being sorted (0=R, 1=G, 2=B)

static int ucolor_cmp(const void *a, const void *b) {
	uint16_t ca = ((const ucolor_t*)a)->c5551, cb = ((const ucolor_t*)b)->c5551;
	int va, vb;
	switch (g_axis) {
	case 0:  va = (ca >> 11) & 0x1F; vb = (cb >> 11) & 0x1F; break;
	case 1:  va = (ca >> 6)  & 0x1F; vb = (cb >> 6)  & 0x1F; break;
	default: va = (ca >> 1)  & 0x1F; vb = (cb >> 1)  & 0x1F; break;
	}
	return va - vb;
}

// Reduce `uni` (the unique-color histogram) to at most `ncol` boxes, writing the
// resulting palette into tlut[] and, for each unique color, its palette index
// into uni2pal[].
static void median_cut(ucolor_t *uni, int nuni, int ncol, uint16_t *tlut, int *uni2pal)
{
	struct box_t { int lo, hi; };
	std::vector<box_t> boxes;
	boxes.push_back({0, nuni});

	while ((int)boxes.size() < ncol) {
		// Pick the box with the widest channel spread and split it.
		int best = -1, best_spread = -1, best_axis = 0;
		for (int i = 0; i < (int)boxes.size(); i++) {
			if (boxes[i].hi - boxes[i].lo <= 1) continue;
			for (int ax = 0; ax < 3; ax++) {
				int mn = 31, mx = 0;
				for (int j = boxes[i].lo; j < boxes[i].hi; j++) {
					int v = (ax == 0) ? (uni[j].c5551 >> 11) & 0x1F :
					        (ax == 1) ? (uni[j].c5551 >> 6)  & 0x1F :
					                    (uni[j].c5551 >> 1)  & 0x1F;
					if (v < mn) mn = v;
					if (v > mx) mx = v;
				}
				if (mx - mn > best_spread) { best_spread = mx - mn; best = i; best_axis = ax; }
			}
		}
		if (best < 0) break;   // every box holds a single color: done

		box_t bx = boxes[best];
		g_axis = best_axis;
		qsort(uni + bx.lo, bx.hi - bx.lo, sizeof(ucolor_t), ucolor_cmp);

		// Split at the weighted median so both halves carry a similar pixel mass.
		uint64_t total = 0;
		for (int j = bx.lo; j < bx.hi; j++) total += uni[j].count;
		uint64_t half = total / 2, acc = 0;
		int mid = bx.lo + 1;
		for (int j = bx.lo; j < bx.hi - 1; j++) {
			acc += uni[j].count;
			if (acc >= half) { mid = j + 1; break; }
		}
		boxes[best] = { bx.lo, mid };
		boxes.push_back({ mid, bx.hi });
	}

	// Emit one palette entry per box (pixel-weighted average color) and record
	// the palette index of every unique color it covers.
	for (int i = 0; i < (int)boxes.size(); i++) {
		uint64_t sr = 0, sg = 0, sb = 0, sa = 0, sn = 0;
		for (int j = boxes[i].lo; j < boxes[i].hi; j++) {
			sr += ((uni[j].c5551 >> 11) & 0x1F) * (uint64_t)uni[j].count;
			sg += ((uni[j].c5551 >> 6)  & 0x1F) * (uint64_t)uni[j].count;
			sb += ((uni[j].c5551 >> 1)  & 0x1F) * (uint64_t)uni[j].count;
			sa += (uni[j].c5551 & 1)            * (uint64_t)uni[j].count;
			sn += uni[j].count;
			uni2pal[j] = i;
		}
		int r5 = sn ? (int)((sr + sn / 2) / sn) : 0;
		int g5 = sn ? (int)((sg + sn / 2) / sn) : 0;
		int b5 = sn ? (int)((sb + sn / 2) / sn) : 0;
		int a1 = (sa * 2 >= sn) ? 1 : 0;   // pixel-weighted majority alpha
		tlut[i] = (uint16_t)((r5 << 11) | (g5 << 6) | (b5 << 1) | a1);
	}
}

// Quantize one RGBA frame to CI8: fill idx[w*h] (palette indices) and tlut[256]
// (RGBA5551). Lossless when the frame has <= flag_gif_colors distinct colors,
// otherwise a median-cut palette is used. Returns true if the frame was lossy.
static bool quantize_frame(const std::vector<uint8_t> &rgba, int w, int h,
	uint8_t *idx, uint16_t *tlut)
{
	size_t npix = (size_t)w * h;
	std::vector<uint16_t> c(npix);
	for (size_t i = 0; i < npix; i++)
		c[i] = conv_rgba5551(rgba[i*4+0], rgba[i*4+1], rgba[i*4+2], rgba[i*4+3]);

	// Build the unique-color histogram. A 5551 value fits in 16 bits, so a flat
	// 64K lookup gives O(1) value -> unique-index mapping with no sorting.
	std::vector<int> seen(65536, -1);
	std::vector<ucolor_t> uni;
	for (size_t i = 0; i < npix; i++) {
		int s = seen[c[i]];
		if (s < 0) {
			seen[c[i]] = (int)uni.size();
			uni.push_back({ c[i], 1u });
		} else {
			uni[s].count++;
		}
	}

	memset(tlut, 0, GIF64_TLUT_BYTES);
	int ncol = flag_gif_colors;
	if (ncol > GIF64_TLUT_COLORS) ncol = GIF64_TLUT_COLORS;

	if ((int)uni.size() <= ncol) {
		// Lossless: one palette slot per distinct color, exact index plane.
		for (int i = 0; i < (int)uni.size(); i++)
			tlut[i] = uni[i].c5551;
		for (size_t i = 0; i < npix; i++)
			idx[i] = (uint8_t)seen[c[i]];
		return false;
	}

	// Lossy: reduce to ncol colors via median cut, then remap every pixel.
	std::vector<int> uni2pal(uni.size());
	median_cut(uni.data(), (int)uni.size(), ncol, tlut, uni2pal.data());
	// median_cut reordered `uni`, so rebuild value -> unique-index first.
	for (int i = 0; i < (int)uni.size(); i++)
		seen[uni[i].c5551] = i;
	for (size_t i = 0; i < npix; i++)
		idx[i] = (uint8_t)uni2pal[seen[c[i]]];
	return true;
}

int gif_convert(const char *infn, const char *outfn)
{
	if (flag_verbose)
		fprintf(stderr, "Converting: %s => %s (%dx%d, %d colors)\n",
			infn, outfn, flag_gif_width, flag_gif_height, flag_gif_colors);

	std::vector<gif_frame_t> frames;
	int gw, gh;
	if (!read_gif(infn, &frames, &gw, &gh))
		return 1;
	if (frames.empty()) {
		fprintf(stderr, "ERROR: %s: no frames decoded\n", infn);
		return 1;
	}

	int w = flag_gif_width, h = flag_gif_height;
	if (flag_verbose)
		fprintf(stderr, "  input: %d frames, %dx%d\n", (int)frames.size(), gw, gh);

	FILE *out = fopen(outfn, "wb");
	if (!out) {
		fprintf(stderr, "ERROR: %s: cannot create file\n", outfn);
		return 1;
	}

	int nframes = (int)frames.size();
	uint16_t flags = 0;
	int lossy = 0;

	// Header (see gif64_internal.h). The flags word is patched after all frames
	// are quantized, once we know whether any of them needed a lossy palette.
	fwrite(GIF64_ID, 1, 4, out);
	w16(out, GIF64_VERSION);
	int flags_pos = ftell(out);
	w16(out, 0);                    // flags placeholder, patched below
	w32(out, nframes);
	w16(out, w);
	w16(out, h);

	// Per-frame GCE delays (ms), clamped to the uint16 range of the delay field.
	for (int i = 0; i < nframes; i++) {
		int d = frames[i].delay_ms;
		if (d < 0) d = 0;
		if (d > 0xFFFF) d = 0xFFFF;
		w16(out, d);
	}

	// Frame planes: CI8 indices followed by the frame's RGBA5551 TLUT.
	std::vector<uint8_t> idx((size_t)w * h);
	uint16_t tlut[GIF64_TLUT_COLORS];
	for (int i = 0; i < nframes; i++) {
		std::vector<uint8_t> rgba = resize_rgba(frames[i].rgba, gw, gh, w, h);
		if (quantize_frame(rgba, w, h, idx.data(), tlut))
			lossy++;
		fwrite(idx.data(), 1, (size_t)w * h, out);
		for (int j = 0; j < GIF64_TLUT_COLORS; j++)
			w16(out, tlut[j]);
	}

	if (lossy) flags |= GIF64_FLAG_LOSSY;
	w16_at(out, flags_pos, flags);

	if (flag_verbose)
		fprintf(stderr, "  wrote %ld bytes, %d/%d frames quantized (lossy)\n",
			ftell(out), lossy, nframes);

	fclose(out);
	return 0;
}
