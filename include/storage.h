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

// Negative error codes.
#define STG_ENOTFOUND  -1
#define STG_EIO        -2
#define STG_EFULL      -3
#define STG_ENOFS      -4    // not mounted / no recognisable filesystem
#define STG_ETOOBIG    -5

#endif
