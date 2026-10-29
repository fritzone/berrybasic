#ifndef _POD_DIRENT_H
#define _POD_DIRENT_H
// Minimal <dirent.h> for the pod-libc, over the CAP_DIRS dir_open/dir_read
// services. One scan is active at a time (a single global cursor in the
// interpreter), so keep at most one DIR open at once.

#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

struct dirent {
    unsigned char d_type;      // DT_DIR or DT_REG
    long          d_size;      // size in bytes (0 for directories) - an extension
    char          d_name[256]; // entry name, NUL-terminated
};

typedef struct DIR DIR;

DIR           *opendir(const char *path);   // 0 if the directory cannot be read
struct dirent *readdir(DIR *d);             // next entry, or 0 at end
int            closedir(DIR *d);            // 0

#endif
