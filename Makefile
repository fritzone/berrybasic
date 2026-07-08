CROSS   = aarch64-linux-gnu-
CC      = $(CROSS)gcc
LD      = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

# Source layout (modular):
#   kernel/   - boot, CPU/MMU, mailbox, UART, core kernel
#   drivers/  - SD, FAT, PCIe, xHCI, USB HID, graphics, fonts, logo
#   basic/    - the BASIC interpreter (shared by target and host)
#   seed/     - native "seed" extension API, backends, runtime, examples
#   include/  - shared platform contracts (console.h, storage.h)
#   host/     - host (Linux) backends for the native test build
KERNEL_DIR  = kernel
DRIVERS_DIR = drivers
BASIC_DIR   = basic
SEED_DIR    = seed
INCLUDE_DIR = include
HOST_DIR    = host
BUILD_DIR   = build

# Header search path shared by every target object.
INCLUDES = -I$(INCLUDE_DIR) -I$(KERNEL_DIR) -I$(DRIVERS_DIR) -I$(BASIC_DIR) -I$(SEED_DIR)

# -mstrict-align: the MMU is disabled early, so all memory is treated as Device
# memory where unaligned 16/32-bit accesses fault. This stops the compiler
# from synthesising unaligned loads/stores (e.g. merging two byte reads of a
# USB descriptor field into one ldrh), which otherwise crashes enumeration.
CFLAGS  = -Wall -O2 -ffreestanding -nostdlib -nostartfiles \
          $(INCLUDES) -mcpu=cortex-a72 -mstrict-align $(CFLAGS_EXTRA)

LDFLAGS = -T $(KERNEL_DIR)/linker.ld

# Target image = kernel + drivers + the interpreter + the seed target backend.
TGT_C = $(wildcard $(KERNEL_DIR)/*.c) $(wildcard $(DRIVERS_DIR)/*.c) \
        $(BASIC_DIR)/basic.c $(SEED_DIR)/seed_target.c
TGT_S = $(wildcard $(KERNEL_DIR)/*.S)

# Objects are flattened into build/ (every source basename is unique).
OBJ = $(addprefix $(BUILD_DIR)/,$(notdir $(TGT_C:.c=.o) $(TGT_S:.S=.o)))

# Let pattern rules find each source in its directory.
vpath %.c $(KERNEL_DIR) $(DRIVERS_DIR) $(BASIC_DIR) $(SEED_DIR)
vpath %.S $(KERNEL_DIR)

ELF    = $(BUILD_DIR)/kernel.elf
KERNEL = $(BUILD_DIR)/kernel8.img

all: $(KERNEL)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(ELF): $(OBJ) | $(BUILD_DIR)
	$(LD) $(LDFLAGS) -o $@ $^

$(KERNEL): $(ELF)
	$(OBJCOPY) $< -O binary $@

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# drivers/image.c compiles the vendored stb_image.h, which needs the third_party
# path and the freestanding <stdlib.h>/<string.h> shims. Disable warnings for the
# third-party header (it is not our code).
$(BUILD_DIR)/image.o: CFLAGS += -Ithird_party -Ithird_party/stb_shim -w

clean:
	rm -rf $(BUILD_DIR)

# Interactively choose the native resolution and font, regenerating
# kernel/buildconfig.h, drivers/font_data.c and the HDMI block in boot/config.txt.
# After this, a plain 'make' builds for the new configuration.
config:
	tools/configure.sh

# Scaffold a new native seed in seed/garden/<name>/ (starter source + a Makefile
# that builds the .sed). Prompts for the name, or: make newseed NAME=blur
newseed:
	@tools/newseed.sh $(NAME)

# SD card image for BASIC LOAD/SAVE. Create a blank FAT16 disk with:
#   make sdcard
SDIMG = sd.img

$(SDIMG):
	dd if=/dev/zero of=$@ bs=1M count=64
	mkfs.fat -F 16 -n BERRYDISK $@

sdcard: $(SDIMG)

# Build a bootable Raspberry Pi 4 SD-card image (firmware + kernel + config.txt)
# that runs BerryBasic on real hardware. See tools/mksdimage.sh and README-realhw.md.
sdimage: $(KERNEL) seeds
	tools/mksdimage.sh

# Interactively flash berrybasic-sd.img to a removable card (lists devices,
# excludes the system disk, confirms before erasing). Avoids flashing the wrong
# disk. Builds the image first if needed.
flash: sdimage
	tools/flashsd.sh

run: $(KERNEL) $(SDIMG)
	qemu-system-aarch64 -M raspi4b -m 2G -kernel $(KERNEL) \
	    -serial stdio -display gtk -device usb-kbd -device usb-mouse \
	    -drive file=berrybasic-sd.img,if=sd,format=raw

# Native build of the interpreter for fast testing/debugging on the host.
# basic/basic.c is shared with the target; only the backends differ.
HOSTCC      = cc
HOST_BIN    = $(BUILD_DIR)/basic_host
HOST_INC    = -I$(INCLUDE_DIR) -I$(BASIC_DIR) -I$(SEED_DIR) -Ithird_party
HOST_SRC    = $(BASIC_DIR)/basic.c $(HOST_DIR)/console_host.c $(HOST_DIR)/storage_host.c \
              $(SEED_DIR)/seed_host.c $(HOST_DIR)/image_host.c $(HOST_DIR)/main.c

host: $(HOST_SRC) | $(BUILD_DIR)
	$(HOSTCC) -Wall -g $(HOST_INC) -o $(HOST_BIN) $(HOST_SRC)

# Native "seeds": position-independent AArch64 blobs that BASIC loads with SEED
# and calls with CALL/CALL$. Built with the same bare-metal toolchain as the
# kernel, linked flat (seed/seed.ld), and gated on having ZERO relocations left
# (any survivor means the seed reached for something it can't relocate).
#   make seeds
# -I.../include gives seeds the freestanding seed libc (<stdlib.h>, <string.h>,
# <ctype.h>); -I.../seed finds "seed.h".
SEED_SRC = $(wildcard $(SEED_DIR)/examples/*.c)
SEED_OUT = $(patsubst $(SEED_DIR)/examples/%.c,$(BUILD_DIR)/seeds/%.sed,$(SEED_SRC))
SEED_CFLAGS = -O2 -ffreestanding -nostdlib -fno-builtin -mcpu=cortex-a72 -mstrict-align \
              -mcmodel=tiny -fno-pic -ffunction-sections -fdata-sections \
              -I$(SEED_DIR)/include -I$(SEED_DIR)

# The seed C library: every .c under seed/runtime/ is linked into each seed, and
# --gc-sections drops whatever a given seed doesn't use.
SEED_RT_SRC = $(wildcard $(SEED_DIR)/runtime/*.c)
SEED_RT_OBJ = $(patsubst $(SEED_DIR)/runtime/%.c,$(BUILD_DIR)/seeds/rt_%.o,$(SEED_RT_SRC))

seeds: $(SEED_OUT)

$(BUILD_DIR)/seeds/rt_%.o: $(SEED_DIR)/runtime/%.c | $(BUILD_DIR)
	@mkdir -p $(BUILD_DIR)/seeds
	$(CC) $(SEED_CFLAGS) -c $< -o $@

$(BUILD_DIR)/seeds/%.sed: $(SEED_DIR)/examples/%.c $(SEED_DIR)/seed.h $(SEED_DIR)/seed.ld $(SEED_RT_OBJ) | $(BUILD_DIR)
	@mkdir -p $(BUILD_DIR)/seeds
	$(CC) $(SEED_CFLAGS) -c $< -o $(BUILD_DIR)/seeds/$*.o
	$(LD) --gc-sections -T $(SEED_DIR)/seed.ld \
	    $(BUILD_DIR)/seeds/$*.o $(SEED_RT_OBJ) -o $(BUILD_DIR)/seeds/$*.elf
	@if $(CROSS)readelf -r $(BUILD_DIR)/seeds/$*.elf | grep -q '^Relocation section'; then \
	    echo "SEED ERROR: $< is not self-contained (unresolved relocations):"; \
	    $(CROSS)readelf -r $(BUILD_DIR)/seeds/$*.elf; rm -f $@; exit 1; fi
	$(OBJCOPY) -O binary $(BUILD_DIR)/seeds/$*.elf $@
	@echo "  built seed $@"

.PHONY: all clean config newseed sdcard sdimage flash run host seeds
