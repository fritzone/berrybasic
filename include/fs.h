#ifndef FS_H
#define FS_H
#include <stdint.h>
#include "storage.h"     // stg_dirent, STG_* error codes
#include "partition.h"   // partition (for blk_log_partition)

// ---------------------------------------------------------------------------
// Filesystem layer: identification + a driver table one level above blockdev.
// Adding a filesystem (exFAT, later) is a new driver file plus one fs_register
// line - not surgery on the mount path. Fixed pools, no malloc.
// ---------------------------------------------------------------------------

#define FS_MAX_VOL 6      // SD root + up to 5 USB volumes

// --- filesystem identification (always, even when we can't mount it) --------
typedef enum {
    FSID_UNKNOWN = 0, FSID_FAT12, FSID_FAT16, FSID_FAT32,
    FSID_EXFAT, FSID_NTFS, FSID_EXT234, FSID_ISO9660
} fsid;

fsid        fs_identify(int blkdev, uint64_t start);   // probe the first sectors
const char *fs_id_name(fsid id);                       // "FAT32", "NTFS", ...
int         fs_mountable(fsid id);                     // 1 if a driver can mount it today

// --- a mounted volume -------------------------------------------------------
// The fs layer owns the volume handles; a driver keeps its concrete state in
// `priv` (e.g. a fat_vol slot) and reaches it back through the handle.
typedef struct fs_vol {
    const struct fs_driver *drv;
    int      blkdev;
    uint64_t start;             // partition start LBA
    void    *priv;              // driver-private state
    char     name[8];           // "sd", "usb"
    int      used;
} fs_vol;

// --- a filesystem driver ----------------------------------------------------
// The op set mirrors the storage.h (stg_*) surface, taking the volume handle in
// place of an implicit global. Phase 1 wires FAT; Phase 4 routes stg_* through
// these by volume. An op may be 0 if the driver does not support it.
typedef struct fs_driver {
    const char *name;                                  // "fat", "exfat"
    int  (*probe  )(int blkdev, uint64_t start);       // 1 = mine, 0 = not
    int  (*mount  )(fs_vol *v, int blkdev, uint64_t start);
    void (*unmount)(fs_vol *v);
    // whole-file + namespace (mirrors stg_read/stg_write/stg_delete/...)
    int  (*read_all )(fs_vol *v, const char *name, char *buf, int max);
    int  (*write_all)(fs_vol *v, const char *name, const char *data, int len);
    int  (*remove )(fs_vol *v, const char *path);
    int  (*mkdir  )(fs_vol *v, const char *path);
    int  (*rmdir  )(fs_vol *v, const char *path);
    int  (*chdir  )(fs_vol *v, const char *path);
    const char *(*cwd)(fs_vol *v);
    void (*dir    )(fs_vol *v);
    int  (*diropen)(fs_vol *v, const char *path);
    int  (*dirnext)(fs_vol *v, stg_dirent *out);
    // byte-level channels (mirrors stg_open/close/getb/putb/...)
    int  (*open )(fs_vol *v, const char *name, int mode);
    int  (*close)(fs_vol *v, int ch);
    int  (*getb )(fs_vol *v, int ch);
    int  (*readn)(fs_vol *v, int ch, void *buf, int n);  // bulk read (fast path)
    int  (*putb )(fs_vol *v, int ch, int byte);
    long (*size )(fs_vol *v, int ch);
    long (*tell )(fs_vol *v, int ch);
    int  (*seek )(fs_vol *v, int ch, long pos);
    int  (*eof  )(fs_vol *v, int ch);
} fs_driver;

int      fs_register(const fs_driver *d);          // each driver calls this at init
int      fs_mount(int blkdev, uint64_t start, const char *volname);  // -> volume index, <0 fail
int      fs_unmount(int vol);
fs_vol  *fs_vol_get(int vol);                      // 0 if absent
int      fs_vol_count(void);

// --- mount points + the stg_* path router (Phase 4, in vfs.c) ---------------
// Graft a mounted volume into the single path tree at `at` ("/" for the SD root,
// "/USB" for a stick). The stg_* surface (storage.h) then routes each path to a
// volume by longest-matching mount prefix; relative paths use the current volume.
int  fs_add_mount(int vol, const char *at);
// Scan a block device, mount its first usable FAT volume, and graft it at `at`.
// Returns the volume index (>=0) or <0. Used for USB sticks (see kernel boot).
int  fs_automount(int blkdev, const char *volname, const char *at);
// Hot-plug helpers: is a volume mounted on this blockdev, and unmount+ungraft it.
int  fs_blkdev_mounted(int blkdev);
int  fs_unmount_blkdev(int blkdev);
int  fs_mount_in_use(const char *at);   // is this mount point already taken?

// Boot-log one partition's identification + size, in the house style, e.g.
//   [BLK] usb0p1: exFAT, 57.2 GB - mounted as usb0
void blk_log_partition(int blkdev, int partidx, fsid id, const partition *p);

#endif
