/* Minimal <stdlib.h> shim for the freestanding (bare-metal) build so that
   stb_image.h can be compiled without a hosted libc. Only what stb references.
   This directory is on the include path for drivers/image.c ONLY. */
#ifndef STB_SHIM_STDLIB_H
#define STB_SHIM_STDLIB_H
#include <stddef.h>   /* size_t */
void *malloc(size_t);
void *realloc(void *, size_t);
void  free(void *);
int   abs(int);
#endif
