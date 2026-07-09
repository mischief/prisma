#ifndef PRISMA_IMAGE_H
#define PRISMA_IMAGE_H

#include <stddef.h>

typedef struct {
	unsigned char *data;
	size_t size;
} ImageBlob;

/*
 * Converts src_path (.svg via nanosvg, .png via libpng, or .jpg/.jpeg via
 * libjpeg) into a 72x72 JPEG blob (encoded via libjpeg), resized and
 * rotated 180 degrees as needed, suitable for uploading to a key --
 * every input format goes through the same normalize pipeline, so
 * a pre-made JPEG that happens to be the wrong size or orientation still
 * comes out correct. Caller must release blob.data with free() when
 * non-NULL. On failure, blob.data is NULL.
 */
ImageBlob convert_image(const char *src_path);

/*
 * Same as convert_image(), but for SVG input the source file's own fill
 * colors are ignored entirely and replaced with (r, g, b) -- the SVG's
 * alpha channel becomes a pure stencil mask. This is for "symbolic" icon
 * sets (GNOME/Adwaita-style monochrome status icons, which hardcode a
 * dark fill like #222222 meant to be recolored by a desktop theme
 * engine rather than shown as-is) -- render them in whatever color
 * actually shows up against the device's black key background instead
 * of needing to know or string-replace the source SVG's specific fill
 * color. Non-SVG input (.png/.jpg/.jpeg) ignores the tint and behaves
 * exactly like convert_image().
 */
ImageBlob convert_image_tinted(const char *src_path, unsigned char r, unsigned char g, unsigned char b);

/*
 * Returns a solid black 72x72 JPEG blob, same ownership rules as
 * convert_image(). Used to clear buttons that have no image= configured,
 * so they don't retain stale content from a previous run.
 */
ImageBlob blank_image(void);

/*
 * Synthesizes a flat 72x72 JPEG of a single RGB color, same ownership
 * rules as convert_image(). Used to build key icons on the fly without an
 * image file on disk.
 */
ImageBlob solid_color_image(unsigned char r, unsigned char g, unsigned char b);

#endif
