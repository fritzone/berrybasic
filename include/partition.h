#ifndef PARTITION_H
#define PARTITION_H
#include <stdint.h>

// ---------------------------------------------------------------------------
// General partition discovery over a blockdev: superfloppy (no table), MBR
// (all four primaries, GPT protective handoff), and GPT (with header CRC32
// check). Returns everything it finds; mount policy lives in the caller.
// ---------------------------------------------------------------------------

#define PART_MAX 8

typedef struct {
    uint64_t start_lba;
    uint64_t nblocks;
    uint8_t  mbr_type;        // MBR type byte; 0 for GPT / superfloppy
    char     gpt_type[37];    // GPT type GUID as text; "" for MBR
    int      is_superfloppy;  // whole device, no table
} partition;

// Scan a block device for partitions. Returns how many were found (0 if the
// device has no recognisable table and no filesystem at LBA 0).
int part_scan(int blkdev, partition *out, int max);

#endif
