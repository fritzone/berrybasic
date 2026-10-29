#ifndef _POD_SYS_STAT_H
#define _POD_SYS_STAT_H
#include <stddef.h>
struct stat { long st_size; int st_mode; };
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_ISREG(m) (((m)&S_IFMT)==S_IFREG)
#define S_ISDIR(m) (((m)&S_IFMT)==S_IFDIR)
int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int mkdir(const char *path, int mode);
#endif
