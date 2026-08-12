#ifndef MSC_H
#define MSC_H

// ---------------------------------------------------------------------------
// USB Mass Storage Class driver (Bulk-Only Transport + a minimal SCSI command
// set). Walks the xHCI device table for enumerated BOT/SCSI sticks (class 0x08,
// subclass 0x06, protocol 0x50), brings each one up (GET MAX LUN, TEST UNIT
// READY, INQUIRY, READ CAPACITY) and registers it as a blockdev so the Phase-1
// partition scanner + FAT driver can read it. Real hardware only: the VL805
// xHCI is not emulated by QEMU. (USB storage Phase 3.)
// ---------------------------------------------------------------------------

// Discover + register every attached USB mass-storage device as a blockdev.
// Returns the number registered (0 if none / not real hardware).
int msc_init(void);

#endif
