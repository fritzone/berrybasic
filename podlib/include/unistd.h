#ifndef _POD_UNISTD_H
#define _POD_UNISTD_H
#include <stddef.h>
long read(int fd, void *buf, unsigned long count);
long write(int fd, const void *buf, unsigned long count);
long lseek(int fd, long off, int whence);
int  close(int fd);
int  unlink(const char *path);
int  isatty(int fd);
char *getcwd(char *buf, unsigned long size);
int   chdir(const char *path);
extern char **environ;
#endif
