#ifndef PCIE_H
#define PCIE_H

#include <stdint.h>

// Bring up the BCM2711 PCIe root complex and the VL805 xHCI controller behind
// it (the chip driving the Pi 4's four USB-A ports). Returns the CPU MMIO
// address of the VL805's registers (the xHCI base), or 0 on failure.
//
// This path only exists on real hardware; QEMU's raspi4b has no PCIe, so this
// returns 0 there (and the DWC2/USB-C keyboard path is used instead).
uintptr_t pcie_init(void);

#endif
