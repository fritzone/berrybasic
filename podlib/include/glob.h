#ifndef _POD_GLOB_H
#define _POD_GLOB_H
#include <stddef.h>
typedef struct { size_t gl_pathc; char **gl_pathv; size_t gl_offs; } glob_t;
#define GLOB_NOMATCH 3
int  glob(const char *pattern, int flags, void *errfn, void *pglob);
void globfree(void *pglob);
#endif
