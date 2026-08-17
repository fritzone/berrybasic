// VFS: the storage.h (stg_*) surface as a path router over mounted volumes.
// The SD card is grafted at "/" and a USB stick at "/USB" (fs_add_mount); each
// caller path is resolved to a volume by longest-matching mount prefix, and the
// call dispatched to that volume's fs_driver ops (fat.c provides them). Relative
// paths use the "current" volume, which CD switches. File channels are global
// handles mapped to (volume, driver-channel) so files on the SD and a stick can
// be open at once. (USB storage Phase 4.)

#include <stdint.h>
#include "fs.h"
#include "storage.h"
#include "partition.h"
#include "blockdev.h"

static int  s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void s_cpy(char *d, const char *s, int max) { int i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }   // FAT is case-insensitive

// --- mount table ------------------------------------------------------------
typedef struct { int vol; char at[24]; } mnt_t;
static mnt_t g_mnt[FS_MAX_VOL];
static int   g_mnt_n;
static int   g_cur = -1;                 // current volume (for relative paths)

int fs_add_mount(int vol, const char *at) {
    if (g_mnt_n >= FS_MAX_VOL || vol < 0 || !at) return -1;
    g_mnt[g_mnt_n].vol = vol;
    s_cpy(g_mnt[g_mnt_n].at, at, (int)sizeof g_mnt[0].at);
    g_mnt_n++;
    if (g_cur < 0) g_cur = vol;          // first mount (the SD root) is the default
    return 0;
}

// Longest mount prefix of an absolute path -> volume + the path within it. Root
// "/" matches everything; "/USB" matches "/USB" and "/USB/...".
static fs_vol *by_prefix(const char *abs, const char **sub) {
    int best = -1, bestlen = -1;
    for (int i = 0; i < g_mnt_n; i++) {
        const char *at = g_mnt[i].at; int L = s_len(at), match = 0;
        if (L == 1 && at[0] == '/') match = 1;                 // root
        else { int j = 0; while (j < L && lc(abs[j]) == lc(at[j])) j++;   // case-insensitive
               if (j == L && (abs[L] == 0 || abs[L] == '/')) match = 1; }
        if (match && L > bestlen) { best = i; bestlen = L; }
    }
    if (best < 0) return 0;
    const char *s = (bestlen == 1) ? abs : abs + bestlen;      // strip the prefix (root strips nothing)
    if (*s == 0) s = "/";
    *sub = s;
    return fs_vol_get(g_mnt[best].vol);
}

// Absolute paths route by mount prefix; relative paths go to the current volume
// unchanged (its driver applies its own cwd).
static fs_vol *route(const char *path, const char **sub) {
    if (path && path[0] == '/') return by_prefix(path, sub);
    *sub = path ? path : "";
    return fs_vol_get(g_cur);
}
#define D(v) ((v)->drv)

// --- whole-file + namespace -------------------------------------------------
int stg_read(const char *name, char *buf, int max) {
    const char *s; fs_vol *v = route(name, &s);
    return (v && D(v)->read_all) ? D(v)->read_all(v, s, buf, max) : STG_ENOFS;
}
int stg_write(const char *name, const char *data, int len) {
    const char *s; fs_vol *v = route(name, &s);
    return (v && D(v)->write_all) ? D(v)->write_all(v, s, data, len) : STG_ENOFS;
}
int stg_delete(const char *name) {
    const char *s; fs_vol *v = route(name, &s);
    return (v && D(v)->remove) ? D(v)->remove(v, s) : STG_ENOFS;
}
int stg_mkdir(const char *path) {
    const char *s; fs_vol *v = route(path, &s);
    return (v && D(v)->mkdir) ? D(v)->mkdir(v, s) : STG_ENOFS;
}
int stg_rmdir(const char *path) {
    const char *s; fs_vol *v = route(path, &s);
    return (v && D(v)->rmdir) ? D(v)->rmdir(v, s) : STG_ENOFS;
}
void stg_dir(void) {
    fs_vol *v = fs_vol_get(g_cur);
    if (v && D(v)->dir) D(v)->dir(v);
}
int stg_chdir(const char *path) {
    const char *s; fs_vol *v = route(path, &s);
    if (!v || !D(v)->chdir) return STG_ENOFS;
    int r = D(v)->chdir(v, s);
    if (r == 0)                                     // success: make it the current volume
        for (int i = 0; i < FS_MAX_VOL; i++) if (fs_vol_get(i) == v) { g_cur = i; break; }
    return r;
}
const char *stg_cwd(void) {
    static char buf[256];
    fs_vol *v = fs_vol_get(g_cur);
    const char *at = "/";
    for (int i = 0; i < g_mnt_n; i++) if (g_mnt[i].vol == g_cur) { at = g_mnt[i].at; break; }
    const char *dc = (v && D(v)->cwd) ? D(v)->cwd(v) : "/";
    if (at[0] == '/' && at[1] == 0) { s_cpy(buf, dc, (int)sizeof buf); return buf; }  // root volume
    int n = 0;                                        // "/USB" + (dc unless it is just "/")
    for (const char *p = at; *p && n < 255; p++) buf[n++] = *p;
    if (!(dc[0] == '/' && dc[1] == 0)) for (const char *p = dc; *p && n < 255; p++) buf[n++] = *p;
    buf[n] = 0;
    return buf;
}

// --- directory enumeration (single global cursor; remember its volume) ------
static fs_vol *g_dirvol;
int stg_diropen(const char *path) {
    const char *s; fs_vol *v = route(path, &s);
    if (!v || !D(v)->diropen) return STG_ENOFS;
    g_dirvol = v;
    return D(v)->diropen(v, s);
}
int stg_dirnext(stg_dirent *out) {
    return (g_dirvol && D(g_dirvol)->dirnext) ? D(g_dirvol)->dirnext(g_dirvol, out) : STG_ENOFS;
}

// --- file channels: global handle -> (volume, driver channel) ---------------
#define STG_MAXCH 16
static struct { fs_vol *v; int dch, used; } g_ch[STG_MAXCH + 1];
static int ch_ok(int g) { return g >= 1 && g <= STG_MAXCH && g_ch[g].used; }

int stg_open(const char *name, int mode) {
    const char *s; fs_vol *v = route(name, &s);
    if (!v || !D(v)->open) return 0;
    int dch = D(v)->open(v, s, mode);
    if (dch <= 0) return 0;
    for (int g = 1; g <= STG_MAXCH; g++) if (!g_ch[g].used) {
        g_ch[g].v = v; g_ch[g].dch = dch; g_ch[g].used = 1;
        return g;
    }
    if (D(v)->close) D(v)->close(v, dch);            // no global slot: undo
    return 0;
}
int stg_close(int ch) {
    if (!ch_ok(ch)) return STG_EBADF;
    fs_vol *v = g_ch[ch].v;
    int r = D(v)->close ? D(v)->close(v, g_ch[ch].dch) : STG_EBADF;
    g_ch[ch].used = 0;
    return r;
}
void stg_close_all(void) { for (int g = 1; g <= STG_MAXCH; g++) if (g_ch[g].used) stg_close(g); }

int stg_getb(int ch) {
    if (!ch_ok(ch)) return STG_EBADF;
    fs_vol *v = g_ch[ch].v; return D(v)->getb ? D(v)->getb(v, g_ch[ch].dch) : STG_EBADF;
}
int stg_readn(int ch, void *buf, int n) {
    if (!ch_ok(ch)) return STG_EBADF;
    fs_vol *v = g_ch[ch].v;
    if (D(v)->readn) return D(v)->readn(v, g_ch[ch].dch, buf, n);
    uint8_t *b = (uint8_t *)buf; int i = 0;          // fallback: byte loop
    for (; i < n; i++) { int c = D(v)->getb ? D(v)->getb(v, g_ch[ch].dch) : -1; if (c < 0) break; b[i] = (uint8_t)c; }
    return i;
}
int stg_putb(int ch, int byte) {
    if (!ch_ok(ch)) return STG_EBADF;
    fs_vol *v = g_ch[ch].v; return D(v)->putb ? D(v)->putb(v, g_ch[ch].dch, byte) : STG_EBADF;
}
long stg_size(int ch) {
    if (!ch_ok(ch)) return STG_EBADF;
    fs_vol *v = g_ch[ch].v; return D(v)->size ? D(v)->size(v, g_ch[ch].dch) : STG_EBADF;
}
long stg_tell(int ch) {
    if (!ch_ok(ch)) return STG_EBADF;
    fs_vol *v = g_ch[ch].v; return D(v)->tell ? D(v)->tell(v, g_ch[ch].dch) : STG_EBADF;
}
int stg_seek(int ch, long pos) {
    if (!ch_ok(ch)) return STG_EBADF;
    fs_vol *v = g_ch[ch].v; return D(v)->seek ? D(v)->seek(v, g_ch[ch].dch, pos) : STG_EBADF;
}
int stg_eof(int ch) {
    if (!ch_ok(ch)) return STG_EBADF;
    fs_vol *v = g_ch[ch].v; return D(v)->eof ? D(v)->eof(v, g_ch[ch].dch) : 1;
}

// --- hot-plug: is anything mounted on this blockdev? ------------------------
int fs_blkdev_mounted(int blkdev) {
    for (int i = 0; i < FS_MAX_VOL; i++) {
        fs_vol *v = fs_vol_get(i);
        if (v && v->blkdev == blkdev) return 1;
    }
    return 0;
}

// Is a mount point already grafted into the tree? (Used to pick a free /USBn for
// each additional stick.) Case-insensitive, matching the path router.
int fs_mount_in_use(const char *at) {
    for (int i = 0; i < g_mnt_n; i++) {
        const char *a = g_mnt[i].at; int j = 0;
        while (a[j] && at[j] && lc(a[j]) == lc(at[j])) j++;
        if (a[j] == 0 && at[j] == 0) return 1;
    }
    return 0;
}

// Hot-plug removal: unmount whatever is mounted on `blkdev` and drop its mount
// point from the tree. Closes any channels open on it, resets the dir cursor, and
// if you were "in" that volume drops you back to the SD root. 0 if unmounted.
int fs_unmount_blkdev(int blkdev) {
    for (int i = 0; i < FS_MAX_VOL; i++) {
        fs_vol *v = fs_vol_get(i);
        if (!v || v->blkdev != blkdev) continue;
        for (int m = 0; m < g_mnt_n; )                    // drop its mount point(s)
            if (g_mnt[m].vol == i) {
                for (int k = m; k < g_mnt_n - 1; k++) g_mnt[k] = g_mnt[k + 1];
                g_mnt_n--;
            } else m++;
        if (g_cur == i) g_cur = (g_mnt_n > 0) ? g_mnt[0].vol : -1;   // leave the gone volume
        for (int g = 1; g <= STG_MAXCH; g++)              // invalidate its open channels
            if (g_ch[g].used && g_ch[g].v == v) g_ch[g].used = 0;
        if (g_dirvol == v) g_dirvol = 0;
        fs_unmount(i);
        return 0;
    }
    return -1;
}

// --- automount a block device's first usable FAT at a mount point -----------
int fs_automount(int blkdev, const char *volname, const char *at) {
    partition p[PART_MAX];
    int n = part_scan(blkdev, p, PART_MAX), chosen = -1;
    for (int i = 0; i < n; i++) {
        fsid id = fs_identify(blkdev, p[i].start_lba);
        blk_log_partition(blkdev, i, id, &p[i]);
        if (fs_mountable(id)) { chosen = i; break; }           // first mountable wins
    }
    if (chosen < 0) return -1;
    int vol = fs_mount(blkdev, p[chosen].start_lba, volname);
    if (vol < 0) return -1;
    fs_add_mount(vol, at);
    return vol;
}
