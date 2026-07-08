// Host storage backend: BASIC LOAD/SAVE map to real files in the current
// directory, so the feature can be tested natively. Mirrors the target FAT
// backend's storage.h API.
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
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

// --- byte-level channels (stdio) --------------------------------------------
#define STG_MAX_FILES 4
static FILE *chan[STG_MAX_FILES];

int stg_open(const char *name, int mode) {
    int idx = -1;
    for (int i = 0; i < STG_MAX_FILES; i++) if (!chan[i]) { idx = i; break; }
    if (idx < 0) return 0;
    const char *m = (mode == STG_M_READ)  ? "rb"
                  : (mode == STG_M_WRITE)  ? "wb+"
                                           : "r+b";   // STG_M_UPDATE
    FILE *f = fopen(name, m);
    if (!f) return 0;                                 // not found / failed
    chan[idx] = f;
    return idx + 1;
}

static FILE *hnd(int ch) {
    if (ch < 1 || ch > STG_MAX_FILES) return 0;
    return chan[ch - 1];
}

int stg_close(int ch) {
    FILE *f = hnd(ch);
    if (!f) return STG_EBADF;
    fclose(f);
    chan[ch - 1] = 0;
    return 0;
}

void stg_close_all(void) {
    for (int i = 0; i < STG_MAX_FILES; i++)
        if (chan[i]) { fclose(chan[i]); chan[i] = 0; }
}

int stg_getb(int ch) {
    FILE *f = hnd(ch);
    if (!f) return STG_EBADF;
    int c = fgetc(f);
    return (c == EOF) ? -1 : c;
}

int stg_putb(int ch, int byte) {
    FILE *f = hnd(ch);
    if (!f) return STG_EBADF;
    return (fputc(byte & 0xFF, f) == EOF) ? STG_EIO : 0;
}

long stg_size(int ch) {
    FILE *f = hnd(ch);
    if (!f) return STG_EBADF;
    long cur = ftell(f);
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    fseek(f, cur, SEEK_SET);
    return end;
}

long stg_tell(int ch) {
    FILE *f = hnd(ch);
    if (!f) return STG_EBADF;
    return ftell(f);
}

int stg_seek(int ch, long pos) {
    FILE *f = hnd(ch);
    if (!f) return STG_EBADF;
    return fseek(f, pos, SEEK_SET) == 0 ? 0 : STG_EIO;
}

int stg_eof(int ch) {
    FILE *f = hnd(ch);
    if (!f) return 1;
    return ftell(f) >= stg_size(ch);
}

void stg_dir(void) {
    DIR *d = opendir(".");
    if (!d) { con_puts("No disk\n"); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        con_puts(e->d_name);
        if (e->d_type == DT_DIR) con_puts("  <DIR>");
        con_putc('\n');
    }
    closedir(d);
}

// --- directories (map to the real filesystem) -------------------------------
int stg_mkdir(const char *path) {
    if (mkdir(path, 0777) == 0) return 0;
    return (errno == EEXIST) ? STG_EEXIST : STG_EIO;
}

int stg_rmdir(const char *path) {
    if (rmdir(path) == 0) return 0;
    if (errno == ENOTEMPTY || errno == EEXIST) return STG_ENOTEMPTY;
    if (errno == ENOENT) return STG_ENOTFOUND;
    return STG_EIO;
}

int stg_chdir(const char *path) {
    return chdir(path) == 0 ? 0 : STG_ENOTFOUND;
}

const char *stg_cwd(void) {
    static char cwdbuf[1024];
    return getcwd(cwdbuf, sizeof cwdbuf) ? cwdbuf : "/";
}
