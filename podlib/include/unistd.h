#ifndef _POD_UNISTD_H
#define _POD_UNISTD_H
#include <stddef.h>
long read(int fd, void *buf, unsigned long count);
long write(int fd, const void *buf, unsigned long count);
long lseek(int fd, long off, int whence);
int  close(int fd);
int  unlink(const char *path);
int  isatty(int fd);
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
int  access(const char *path, int mode);   // 0 if the file exists, else -1
char *getcwd(char *buf, unsigned long size);
int   chdir(const char *path);
extern char **environ;
#endif
