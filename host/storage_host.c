// Host storage backend: BASIC LOAD/SAVE map to real files in the current
// directory, so the feature can be tested natively. Mirrors the target FAT
// backend's storage.h API.
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "storage.h"
#include "console.h"

int stg_init(void) { return 0; }

int stg_read(const char *name, char *buf, int maxlen) {
    FILE *f = fopen(name, "rb");
    if (!f) return STG_ENOTFOUND;
    int n = (int)fread(buf, 1, maxlen, f);
    fclose(f);
    return n;
}

int stg_write(const char *name, const char *data, int len) {
    FILE *f = fopen(name, "wb");
    if (!f) return STG_EIO;
    int n = (int)fwrite(data, 1, len, f);
    fclose(f);
    return (n == len) ? 0 : STG_EIO;
}

int stg_delete(const char *name) {
    return remove(name) == 0 ? 0 : STG_ENOTFOUND;
}

void stg_dir(void) {
    DIR *d = opendir(".");
    if (!d) { con_puts("No disk\n"); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        con_puts(e->d_name);
        con_putc('\n');
    }
    closedir(d);
}
