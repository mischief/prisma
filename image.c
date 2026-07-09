#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#include <nanosvg/nanosvg.h>
#include <nanosvg/nanosvgrast.h>
#include <png.h>
#include <jpeglib.h>

#include "image.h"

extern int debug;

#define TARGET_SIZE 72

typedef struct {
	unsigned char *pixels; /* RGB, 3 bytes/pixel, row-major, top-to-bottom */
	int width;
	int height;
} RGBImage;

static void
rgb_free(RGBImage *img)
{
	free(img->pixels);
	img->pixels = NULL;
}

static int
has_suffix(const char *path, const char *suffix)
{
	size_t plen = strlen(path);
	size_t slen = strlen(suffix);
	if (slen > plen)
		return 0;
	return strcasecmp(path + plen - slen, suffix) == 0;
}

/*
 * Rasterizes non-premultiplied RGBA into an RGB buffer, flattening onto a
 * black background. Since black is (0,0,0), alpha-compositing a pixel onto
 * it collapses to out = in_rgb * alpha / 255 -- no separate blend step
 * needed.
 *
 * If tint is non-NULL, the source pixel's own RGB is ignored entirely and
 * replaced with tint -- the alpha channel becomes a pure stencil mask.
 * This is what lets us render "symbolic" icon sets (GNOME/Adwaita-style
 * monochrome SVGs, which hardcode a dark fill like #222222 meant to be
 * recolored by a desktop theme engine, not shown as-is) in any color: we
 * don't care what color the SVG author picked, only which pixels are
 * opaque.
 */
static void
flatten_onto_black(const unsigned char *rgba, unsigned char *rgb, int n, const unsigned char *tint)
{
	int i;
	for (i = 0; i < n; i++) {
		unsigned char a = rgba[i * 4 + 3];
		unsigned char sr = tint ? tint[0] : rgba[i * 4 + 0];
		unsigned char sg = tint ? tint[1] : rgba[i * 4 + 1];
		unsigned char sb = tint ? tint[2] : rgba[i * 4 + 2];
		rgb[i * 3 + 0] = (unsigned char)((sr * a) / 255);
		rgb[i * 3 + 1] = (unsigned char)((sg * a) / 255);
		rgb[i * 3 + 2] = (unsigned char)((sb * a) / 255);
	}
}

/*
 * nanosvg's rasterizer re-tessellates vector paths at whatever pixel
 * resolution you ask for, with proper scanline anti-aliasing computed at
 * that resolution (like stb_truetype) -- unlike librsvg-via-ImageMagick,
 * which rasterizes at the SVG's declared native size (often a tiny 16x16
 * for system icons) and only then scales the raster up, producing a
 * blurry/blocky result. That means we can rasterize straight to
 * TARGET_SIZE here with no supersample-then-downscale trick needed.
 */
static int
load_svg(const char *path, RGBImage *out, const unsigned char *tint)
{
	NSVGimage *image;
	NSVGrasterizer *rast;
	unsigned char *rgba;
	float sx, sy, scale, tx, ty;

	image = nsvgParseFromFile(path, "px", 96);
	if (!image) {
		fprintf(stderr, "Failed to parse SVG %s\n", path);
		return -1;
	}
	if (image->width <= 0 || image->height <= 0) {
		fprintf(stderr, "SVG %s has invalid dimensions\n", path);
		nsvgDelete(image);
		return -1;
	}

	rast = nsvgCreateRasterizer();
	if (!rast) {
		fprintf(stderr, "nsvgCreateRasterizer failed\n");
		nsvgDelete(image);
		return -1;
	}

	rgba = malloc((size_t)TARGET_SIZE * TARGET_SIZE * 4);
	if (!rgba) {
		fprintf(stderr, "malloc failed\n");
		nsvgDeleteRasterizer(rast);
		nsvgDelete(image);
		return -1;
	}
	memset(rgba, 0, (size_t)TARGET_SIZE * TARGET_SIZE * 4);

	/* Uniform scale to fit the SVG's own viewbox into our square
	 * canvas, preserving aspect ratio, centered. */
	sx = (float)TARGET_SIZE / image->width;
	sy = (float)TARGET_SIZE / image->height;
	scale = sx < sy ? sx : sy;
	tx = (TARGET_SIZE - image->width * scale) * 0.5f;
	ty = (TARGET_SIZE - image->height * scale) * 0.5f;

	nsvgRasterizeXY(rast, image, tx, ty, scale, scale, rgba,
	                TARGET_SIZE, TARGET_SIZE, TARGET_SIZE * 4);

	nsvgDeleteRasterizer(rast);
	nsvgDelete(image);

	out->pixels = malloc((size_t)TARGET_SIZE * TARGET_SIZE * 3);
	if (!out->pixels) {
		fprintf(stderr, "malloc failed\n");
		free(rgba);
		return -1;
	}
	flatten_onto_black(rgba, out->pixels, TARGET_SIZE * TARGET_SIZE, tint);
	free(rgba);

	out->width = TARGET_SIZE;
	out->height = TARGET_SIZE;

	if (debug)
		fprintf(stderr, "SVG %s rasterized directly to %dx%d\n",
		        path, TARGET_SIZE, TARGET_SIZE);

	return 0;
}

static int
load_png(const char *path, RGBImage *out)
{
	png_image pimg;
	unsigned char *rgba;

	memset(&pimg, 0, sizeof(pimg));
	pimg.version = PNG_IMAGE_VERSION;

	if (!png_image_begin_read_from_file(&pimg, path)) {
		fprintf(stderr, "Failed to read PNG %s: %s\n", path, pimg.message);
		return -1;
	}

	pimg.format = PNG_FORMAT_RGBA;

	rgba = malloc(PNG_IMAGE_SIZE(pimg));
	if (!rgba) {
		fprintf(stderr, "malloc failed\n");
		png_image_free(&pimg);
		return -1;
	}

	if (!png_image_finish_read(&pimg, NULL, rgba, 0, NULL)) {
		fprintf(stderr, "Failed to decode PNG %s: %s\n", path, pimg.message);
		free(rgba);
		png_image_free(&pimg);
		return -1;
	}

	out->width = (int)pimg.width;
	out->height = (int)pimg.height;
	out->pixels = malloc((size_t)out->width * out->height * 3);
	if (!out->pixels) {
		fprintf(stderr, "malloc failed\n");
		free(rgba);
		png_image_free(&pimg);
		return -1;
	}

	flatten_onto_black(rgba, out->pixels, out->width * out->height, NULL);

	free(rgba);
	png_image_free(&pimg);

	if (debug)
		fprintf(stderr, "PNG %s decoded to %dx%d\n", path, out->width, out->height);

	return 0;
}

/*
 * Existing JPEGs (e.g. a pre-made icon someone hands us directly) used to
 * get uploaded byte-for-byte as-is, with no check that they were even
 * 72x72 or correctly rotated -- silently wrong for anything that wasn't
 * hand-crafted to match the device's exact expectations. Decoding them
 * here and running them through the same resize/rotate/re-encode
 * pipeline as SVG/PNG makes every input format behave identically.
 */
static int
load_jpeg(const char *path, RGBImage *out)
{
	struct jpeg_decompress_struct cinfo;
	struct jpeg_error_mgr jerr;
	FILE *f;
	JSAMPROW row_pointer[1];
	int row_stride;

	f = fopen(path, "rbe"); /* "e" -> O_CLOEXEC */
	if (!f) {
		fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
		return -1;
	}

	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_decompress(&cinfo);
	jpeg_stdio_src(&cinfo, f);

	if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
		fprintf(stderr, "Failed to read JPEG header from %s\n", path);
		jpeg_destroy_decompress(&cinfo);
		fclose(f);
		return -1;
	}

	cinfo.out_color_space = JCS_RGB;

	if (!jpeg_start_decompress(&cinfo)) {
		fprintf(stderr, "Failed to start JPEG decompression for %s\n", path);
		jpeg_destroy_decompress(&cinfo);
		fclose(f);
		return -1;
	}

	out->width = (int)cinfo.output_width;
	out->height = (int)cinfo.output_height;
	out->pixels = malloc((size_t)out->width * out->height * 3);
	if (!out->pixels) {
		fprintf(stderr, "malloc failed\n");
		jpeg_abort_decompress(&cinfo);
		jpeg_destroy_decompress(&cinfo);
		fclose(f);
		return -1;
	}

	row_stride = out->width * cinfo.output_components;
	while (cinfo.output_scanline < cinfo.output_height) {
		row_pointer[0] = out->pixels + (size_t)cinfo.output_scanline * row_stride;
		jpeg_read_scanlines(&cinfo, row_pointer, 1);
	}

	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	fclose(f);

	if (debug)
		fprintf(stderr, "JPEG %s decoded to %dx%d\n", path, out->width, out->height);

	return 0;
}

/*
 * General-purpose bilinear resize, used to bring whatever native
 * resolution a PNG came in at to exactly TARGET_SIZE x TARGET_SIZE. (The
 * SVG path never needs this -- see load_svg above.)
 */
static int
resize_bilinear(const RGBImage *src, RGBImage *dst, int dst_w, int dst_h)
{
	int x, y, c;

	dst->pixels = malloc((size_t)dst_w * dst_h * 3);
	if (!dst->pixels) {
		fprintf(stderr, "malloc failed\n");
		return -1;
	}
	dst->width = dst_w;
	dst->height = dst_h;

	for (y = 0; y < dst_h; y++) {
		float sy_f = ((float)y + 0.5f) * src->height / dst_h - 0.5f;
		int y0 = (int)floorf(sy_f);
		float fy = sy_f - (float)y0;
		int y1 = y0 + 1;

		if (y0 < 0) y0 = 0;
		if (y0 >= src->height) y0 = src->height - 1;
		if (y1 < 0) y1 = 0;
		if (y1 >= src->height) y1 = src->height - 1;

		for (x = 0; x < dst_w; x++) {
			float sx_f = ((float)x + 0.5f) * src->width / dst_w - 0.5f;
			int x0 = (int)floorf(sx_f);
			float fx = sx_f - (float)x0;
			int x1 = x0 + 1;

			if (x0 < 0) x0 = 0;
			if (x0 >= src->width) x0 = src->width - 1;
			if (x1 < 0) x1 = 0;
			if (x1 >= src->width) x1 = src->width - 1;

			for (c = 0; c < 3; c++) {
				float p00 = src->pixels[(y0 * src->width + x0) * 3 + c];
				float p10 = src->pixels[(y0 * src->width + x1) * 3 + c];
				float p01 = src->pixels[(y1 * src->width + x0) * 3 + c];
				float p11 = src->pixels[(y1 * src->width + x1) * 3 + c];
				float top = p00 + (p10 - p00) * fx;
				float bot = p01 + (p11 - p01) * fx;
				float v = top + (bot - top) * fy;
				dst->pixels[(y * dst_w + x) * 3 + c] = (unsigned char)(v + 0.5f);
			}
		}
	}

	return 0;
}

/*
 * Rotating 180 degrees reverses both row order and column order at once,
 * which is exactly the same as reversing the flat pixel array.
 */
static void
rotate_180(const RGBImage *src, RGBImage *dst)
{
	int n = src->width * src->height;
	int i;

	for (i = 0; i < n; i++) {
		int si = n - 1 - i;
		dst->pixels[i * 3 + 0] = src->pixels[si * 3 + 0];
		dst->pixels[i * 3 + 1] = src->pixels[si * 3 + 1];
		dst->pixels[i * 3 + 2] = src->pixels[si * 3 + 2];
	}
}

/*
 * Encodes an RGB buffer as a baseline JPEG at the given quality. Unlike
 * ImageMagick's JPEG coder, libjpeg's basic API never second-guesses the
 * color space based on pixel content -- we set JCS_RGB / 3 components
 * explicitly and that is exactly what gets written, so there's no risk of
 * silently getting a 1-component grayscale JPEG for an achromatic
 * white-on-black icon (see the extensive fight with ImageMagick over
 * exactly this in image_im.c).
 *
 * blob.data on success is a malloc'd buffer (libjpeg's memory destination
 * manager uses malloc/realloc internally); the caller must free() it.
 */
static ImageBlob
encode_jpeg(const unsigned char *rgb, int width, int height, int quality)
{
	ImageBlob blob = {NULL, 0};
	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr jerr;
	unsigned char *outbuffer = NULL;
	unsigned long outsize = 0;
	JSAMPROW row_pointer[1];
	int row_stride;

	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&cinfo);
	jpeg_mem_dest(&cinfo, &outbuffer, &outsize);

	cinfo.image_width = (JDIMENSION)width;
	cinfo.image_height = (JDIMENSION)height;
	cinfo.input_components = 3;
	cinfo.in_color_space = JCS_RGB;

	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, quality, TRUE);

	jpeg_start_compress(&cinfo, TRUE);

	row_stride = width * 3;
	while (cinfo.next_scanline < cinfo.image_height) {
		row_pointer[0] = (JSAMPROW)&rgb[cinfo.next_scanline * row_stride];
		jpeg_write_scanlines(&cinfo, row_pointer, 1);
	}

	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);

	blob.data = outbuffer;
	blob.size = outsize;
	return blob;
}

ImageBlob
blank_image(void)
{
	ImageBlob blob = {NULL, 0};
	unsigned char *rgb;

	rgb = calloc((size_t)TARGET_SIZE * TARGET_SIZE, 3); /* all-zero -> solid black */
	if (!rgb) {
		fprintf(stderr, "malloc failed\n");
		return blob;
	}

	blob = encode_jpeg(rgb, TARGET_SIZE, TARGET_SIZE, 100);
	free(rgb);

	if (!blob.data)
		fprintf(stderr, "JPEG encoding failed for blank image\n");

	return blob;
}

/*
 * Synthesizes a flat TARGET_SIZE x TARGET_SIZE JPEG of a single RGB color
 * -- no rotation needed since a solid color is invariant under it. Used by
 * prisma.color() in the Lua config to build key icons on the fly without an
 * image file on disk (status swatches, level meters, etc).
 */
ImageBlob
solid_color_image(unsigned char r, unsigned char g, unsigned char b)
{
	ImageBlob blob = {NULL, 0};
	unsigned char *rgb;
	int i, n = TARGET_SIZE * TARGET_SIZE;

	rgb = malloc((size_t)n * 3);
	if (!rgb) {
		fprintf(stderr, "malloc failed\n");
		return blob;
	}

	for (i = 0; i < n; i++) {
		rgb[i * 3 + 0] = r;
		rgb[i * 3 + 1] = g;
		rgb[i * 3 + 2] = b;
	}

	blob = encode_jpeg(rgb, TARGET_SIZE, TARGET_SIZE, 100);
	free(rgb);

	if (!blob.data)
		fprintf(stderr, "JPEG encoding failed for solid color image\n");

	return blob;
}

/*
 * Shared implementation behind convert_image() and convert_image_tinted().
 * tint is NULL for the normal (respect the source file's own colors)
 * path; non-NULL only makes sense for SVG input (see flatten_onto_black
 * above) -- PNG/JPEG sources ignore it, since there's no equivalent
 * "symbolic icon, recolor by alpha mask" convention for raster formats.
 */
static ImageBlob
convert_image_impl(const char *src_path, const unsigned char *tint)
{
	ImageBlob blob = {NULL, 0};
	RGBImage loaded = {0};
	RGBImage resized = {0};
	RGBImage rotated = {0};
	int rc;

	if (has_suffix(src_path, ".svg")) {
		rc = load_svg(src_path, &loaded, tint);
	} else if (has_suffix(src_path, ".png")) {
		rc = load_png(src_path, &loaded);
	} else if (has_suffix(src_path, ".jpg") || has_suffix(src_path, ".jpeg")) {
		rc = load_jpeg(src_path, &loaded);
	} else {
		fprintf(stderr, "Unsupported image format for %s "
		                "(only .svg, .png, and .jpg/.jpeg are supported)\n", src_path);
		return blob;
	}

	if (rc < 0)
		return blob;

	if (loaded.width != TARGET_SIZE || loaded.height != TARGET_SIZE) {
		if (resize_bilinear(&loaded, &resized, TARGET_SIZE, TARGET_SIZE) < 0) {
			rgb_free(&loaded);
			return blob;
		}
		rgb_free(&loaded);
	} else {
		resized = loaded;
	}

	rotated.width = TARGET_SIZE;
	rotated.height = TARGET_SIZE;
	rotated.pixels = malloc((size_t)TARGET_SIZE * TARGET_SIZE * 3);
	if (!rotated.pixels) {
		fprintf(stderr, "malloc failed\n");
		rgb_free(&resized);
		return blob;
	}
	rotate_180(&resized, &rotated);
	rgb_free(&resized);

	blob = encode_jpeg(rotated.pixels, TARGET_SIZE, TARGET_SIZE, 100);
	rgb_free(&rotated);

	if (!blob.data) {
		fprintf(stderr, "JPEG encoding failed for %s\n", src_path);
		return blob;
	}

	if (debug) {
		fprintf(stderr, "converted %s (%dx%d, %zu bytes)\n",
		        src_path, TARGET_SIZE, TARGET_SIZE, blob.size);
		FILE *f = fopen("/tmp/prisma_debug.jpg", "wbe"); /* "e" -> O_CLOEXEC */
		if (f) {
			fwrite(blob.data, 1, blob.size, f);
			fclose(f);
			fprintf(stderr, "debug: dumped to /tmp/prisma_debug.jpg\n");
		}
	}

	return blob;
}

ImageBlob
convert_image(const char *src_path)
{
	return convert_image_impl(src_path, NULL);
}

ImageBlob
convert_image_tinted(const char *src_path, unsigned char r, unsigned char g, unsigned char b)
{
	unsigned char tint[3] = {r, g, b};
	return convert_image_impl(src_path, tint);
}
