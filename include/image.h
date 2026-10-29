#ifndef IMAGE_H
#define IMAGE_H

// Image loading for BASIC. Decodes an image file (PNG/JPEG/BMP) from storage
// into the sprite format used by GPUT/con_sprite_put:
//   [4 bytes width LE][4 bytes height LE][width*height pixels, 4 bytes each]
// with each pixel stored little-endian as bytes R,G,B,A (framebuffer order). The
// alpha byte drives GPUT transparency; opaque images decode to A=0xFF.
//
// The decoded sprite lives in a managed pool; the returned value is its address,
// suitable to pass straight to GPUT (or SPRW/SPRH). Returns 0 on any failure
// (file missing, unsupported/corrupt image, too large for the pool). The pool is
// emptied by img_sprite_reset() (called when a BASIC program is cleared/run).
long img_load_sprite(const char *filename);
void img_sprite_reset(void);

// Save a sprite (as produced by LOADSPRITE or GGET, at address `addr`) to an
// image file. The format is PNG unless the name ends in ".bmp". Returns 0 on
// success or one of the negative codes below.
int  img_save_sprite(const char *filename, long addr);

#define IMG_EARG     -1   // null/invalid sprite address or bad dimensions
#define IMG_ETOOBIG  -2   // sprite too large to encode in the work buffer
#define IMG_EENCODE  -3   // stb failed to encode the image
#define IMG_EIO      -4   // storage write failed

#endif
