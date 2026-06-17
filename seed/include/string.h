#ifndef SEED_STRING_H
#define SEED_STRING_H
// Freestanding <string.h> for native seeds, implemented by the seed runtime.
#include <stddef.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

// memory
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);

// length / compare
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);

// copy / concat
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);

// search
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);

// duplicate (uses the seed heap; free with free())
char *strdup(const char *s);
char *strndup(const char *s, size_t n);

#endif // SEED_STRING_H
