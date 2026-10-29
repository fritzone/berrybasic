/* Minimal <string.h> shim for the freestanding build (see stdlib.h shim). The
   mem* functions are defined in kernel/util.c; str* in drivers/image.c. */
#ifndef STB_SHIM_STRING_H
#define STB_SHIM_STRING_H
#include <stddef.h>   /* size_t */
void  *memcpy(void *, const void *, size_t);
void  *memmove(void *, const void *, size_t);
void  *memset(void *, int, size_t);
int    memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
int    strcmp(const char *, const char *);
int    strncmp(const char *, const char *, size_t);
#endif
