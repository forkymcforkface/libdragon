/*
    gifconv64: convert animated GIFs to the predecoded CI8 stream format used by
    the Libdragon SDK

    This tool is part of the Libdragon SDK.

    This is free and unencumbered software released into the public domain.

    For more information, please refer to <http://unlicense.org/>
*/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>

#ifdef __cplusplus
#define _Static_assert static_assert
#endif

#include "../common/binout.c"
#include "../common/binout.h"
#include "../common/polyfill.h"

bool flag_verbose = false;
static bool had_error = false;

/************************************************************************************
 *  CONVERTERS
 ************************************************************************************/

#include "conv_gif64.cpp"

/************************************************************************************
 *  MAIN
 ************************************************************************************/

void usage(void) {
	printf("gifconv64 -- Animated GIF conversion tool for libdragon\n");
	printf("\n");
	printf("Usage:\n");
	printf("   gifconv64 [flags] <file-or-dir> [[flags] <file-or-dir>..]\n");
	printf("\n");
	printf("Supported conversions:\n");
	printf("   * GIF => GIF64 (predecoded CI8 animation stream)\n");
	printf("\n");
	printf("A GIF64 file is an animated GIF decoded offline into a stream of CI8\n");
	printf("frames (an 8-bit palette-index plane plus a 256-entry RGBA5551 TLUT per\n");
	printf("frame, and each frame's delay). The N64 plays it back with no on-device\n");
	printf("LZW decoding, streaming each frame straight off storage.\n");
	printf("\n");
	printf("Global options:\n");
	printf("   -o / --output <dir>       Specify output directory\n");
	printf("   -v / --verbose            Verbose mode\n");
	printf("   -h / --help               Show this help message\n");
	printf("\n");
	printf("GIF options:\n");
	printf("   -s / --size <WxH>         Target frame size (default: 320x240)\n");
	printf("   -c / --colors <N>         Palette cap per frame, 2..256 (default: 256).\n");
	printf("                             256 keeps frames with <=256 colors lossless;\n");
	printf("                             a lower cap median-cuts them (lossy).\n");
	printf("\n");
	printf("Output is always CI8 (a 256-entry TLUT per frame). CI4 is not emitted:\n");
	printf("--colors only bounds how many of the 256 slots are used.\n");
	printf("\n");
}

char* changeext(const char* fn, const char *ext) {
	char buf[4096];
	strcpy(buf, fn);
	*strrchr(buf, '.') = '\0';
	strcat(buf, ext);
	return strdup(buf);
}

void convert(const char *infn, const char *outfn1) {
	const char *ext = strrchr(infn, '.');
	if (!ext) {
		fprintf(stderr, "unknown file type: %s\n", infn);
		had_error = true;
		return;
	}

	if (strcasecmp(ext, ".gif") == 0) {
		char *outfn = changeext(outfn1, ".gif64");
		if (gif_convert(infn, outfn) != 0) had_error = true;
		free(outfn);
	} else {
		fprintf(stderr, "WARNING: ignoring unknown file: %s\n", infn);
	}
}

bool exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0;
}

bool isfile(const char *path) {
	struct stat st;
	stat(path, &st);
	return (st.st_mode & S_IFREG) != 0;
}

bool isdir(const char *path) {
	struct stat st;
	stat(path, &st);
	return (st.st_mode & S_IFDIR) != 0;
}

void walkdir(char *inpath, const char *outpath, void (*func)(const char *, const char*)) {
	if (isdir(inpath)) {
		// We're walking a directory. Make sure there's also a matching
		// output directory or create it otherwise.
		if (!isdir(outpath)) {
			// If there's an obstructing file, exit with an error.
			if (isfile(outpath)) {
				fprintf(stderr, "ERROR: %s is a file but should be a directory\n", outpath);
				had_error = true;
				return;
			}
			mkdir(outpath, 0777);
		}
		DIR* d = opendir(inpath);
		struct dirent *de;
		while ((de = readdir(d))) {
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
				continue;
			char *inpathsub, *outpathsub;
			asprintf(&inpathsub, "%s/%s", inpath, de->d_name);
			asprintf(&outpathsub, "%s/%s", outpath, de->d_name);
			walkdir(inpathsub, outpathsub, func);
			free(inpathsub);
			free(outpathsub);
		}
		closedir(d);
	} else if (isfile(inpath)) {
		if (isdir(outpath)) {
			// We support the format "gifconv64 -o <dir> <file>" as special case
			char *outpathsub;
			char *basename = strrchr(inpath, '/');
			if (!basename) basename = inpath;
			asprintf(&outpathsub, "%s/%s", outpath, basename);

			func(inpath, outpathsub);

			free(outpathsub);
		} else {
			func(inpath, outpath);
		}
	} else {
		fprintf(stderr, "WARNING: ignoring special file: %s\n", inpath);
	}
}

int main(int argc, char *argv[]) {
	winconsole_utf8();

	if (argc < 2) {
		usage();
		return 1;
	}

	const char *outdir = ".";

	int i;
	for (i=1; i<argc; i++) {
		if (argv[i][0] == '-') {
			if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
				flag_verbose = true;
			} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
				usage();
				return 0;
			} else if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) {
				if (++i == argc) {
					fprintf(stderr, "missing argument for -o/--output\n");
					return 1;
				}
				outdir = argv[i];
			} else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--size")) {
				if (++i == argc) {
					fprintf(stderr, "missing argument for -s/--size\n");
					return 1;
				}
				int w, h; char extra;
				if (sscanf(argv[i], "%dx%d%c", &w, &h, &extra) != 2 || w < 1 || h < 1 || w > 65535 || h > 65535) {
					fprintf(stderr, "invalid argument for -s/--size: %s (expected WxH, each 1..65535)\n", argv[i]);
					return 1;
				}
				flag_gif_width = w;
				flag_gif_height = h;
			} else if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--colors")) {
				if (++i == argc) {
					fprintf(stderr, "missing argument for -c/--colors\n");
					return 1;
				}
				flag_gif_colors = atoi(argv[i]);
				if (flag_gif_colors < 2 || flag_gif_colors > 256) {
					fprintf(stderr, "invalid argument for -c/--colors: %s (expected 2..256)\n", argv[i]);
					return 1;
				}
			} else {
				fprintf(stderr, "invalid option: %s\n", argv[i]);
				return 1;
			}
		} else {
			// Positional argument. It's either a file or a directory. Convert it
			if (!exists(argv[i])) {
				fprintf(stderr, "ERROR: file %s does not exist\n", argv[i]);
				had_error = true;
			} else {
				walkdir(argv[i], outdir, convert);
			}
		}
	}

	return had_error ? 1 : 0;
}
