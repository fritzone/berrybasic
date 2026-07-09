#ifndef STORAGE_H
#define STORAGE_H

// File storage for BASIC LOAD / SAVE / CAT / DELETE. Two backends implement it:
//   target: FAT16/FAT32 on the SD card (fat.c over sd.c), root directory only,
//           8.3 filenames.
//   host:   real files in the current directory via stdio (host/storage_host.c),
//           so LOAD/SAVE can be tested natively.

int  stg_init(void);                                      // mount; 0 ok, <0 fail
int  stg_read(const char *name, char *buf, int maxlen);   // -> bytes read, <0 error
int  stg_write(const char *name, const char *data, int len); // 0 ok, <0 error
int  stg_delete(const char *name);                        // 0 ok, <0 error
void stg_dir(void);                                       // print a listing via con_puts

// --- directories ------------------------------------------------------------
// All of the calls above accept paths ("SUB/FILE.EXT", absolute with a leading
// '/', or relative to the current directory). These manage directories:
int  stg_mkdir(const char *path);   // create a directory; 0 ok, <0 error
int  stg_rmdir(const char *path);   // remove an EMPTY directory; 0 ok, <0 error
int  stg_chdir(const char *path);   // change the current directory; 0 ok, <0 error
const char *stg_cwd(void);          // current directory path (for PWD / prompts)

// --- directory enumeration --------------------------------------------------
// One directory scan is active at a time (a single global cursor). stg_diropen
// points it at a directory (path, absolute or relative; "" or "." = current);
// each stg_dirnext yields the next entry. This backs the BASIC DIROPEN /
// DIRNEXT / DIRNAME$ / DIRSIZE / DIRTYPE / DIRDATE$ / DIRTIME$ words.
typedef struct {
    char name[13];              // "NAME.EXT" (8.3, uppercased, no path), NUL-terminated
    int  is_dir;                // 1 = directory, 0 = regular file
    long size;                  // size in bytes (0 for directories)
    int  year, month, day;      // last-write date (month/day 1-based; 0 if none)
    int  hour, minute;          // last-write time
} stg_dirent;

int  stg_diropen(const char *path);   // begin scanning a directory; 0 ok, <0 error
int  stg_dirnext(stg_dirent *out);    // 1 = entry filled, 0 = end of dir, <0 error

// --- byte-level channel (file-handle) I/O -----------------------------------
// Streaming access to individual files, backed directly by the filesystem
// (writes grow the file live). Used by the BASIC OPENIN/OPENOUT/OPENUP, CLOSE#,
// BGET#, BPUT#, EOF#, PTR#, EXT# words. Channels are small positive integers.

// Open modes for stg_open.
#define STG_M_READ    0    // OPENIN : existing file, read only
#define STG_M_WRITE   1    // OPENOUT: create/truncate, read+write
#define STG_M_UPDATE  2    // OPENUP : existing file, read+write

int  stg_open(const char *name, int mode);   // -> channel (>0), or 0 if it failed
int  stg_close(int ch);                       // 0 ok, <0 error (flushes writes)
void stg_close_all(void);                     // CLOSE#0: close every open channel
int  stg_getb(int ch);                        // next byte 0..255, or <0 at EOF/error
int  stg_putb(int ch, int byte);              // 0 ok, <0 error
long stg_size(int ch);                        // EXT#: length in bytes, <0 error
long stg_tell(int ch);                        // PTR#: current position, <0 error
int  stg_seek(int ch, long pos);              // PTR#=: set position, 0 ok, <0 error
int  stg_eof(int ch);                         // 1 at end-of-file, else 0

// Negative error codes.
#define STG_ENOTFOUND  -1
#define STG_EIO        -2
#define STG_EFULL      -3
#define STG_ENOFS      -4    // not mounted / no recognisable filesystem
#define STG_ETOOBIG    -5
#define STG_EBADF      -6    // bad / unopened channel
#define STG_EMFILE     -7    // too many open channels
#define STG_EEXIST     -8    // name already exists (MKDIR)
#define STG_ENOTEMPTY  -9    // directory not empty (RMDIR)

#endif
