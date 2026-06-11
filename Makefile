CROSS   = aarch64-linux-gnu-
CC      = $(CROSS)gcc
LD      = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

SRC_DIR   = src
BUILD_DIR = build

# -mstrict-align: the MMU is disabled, so all memory is treated as Device
# memory where unaligned 16/32-bit accesses fault. This stops the compiler
# from synthesising unaligned loads/stores (e.g. merging two byte reads of a
# USB descriptor field into one ldrh), which otherwise crashes enumeration.
CFLAGS  = -Wall -O2 -ffreestanding -nostdlib -nostartfiles \
          -I$(SRC_DIR) -mcpu=cortex-a72 -mstrict-align $(CFLAGS_EXTRA)

LDFLAGS = -T $(SRC_DIR)/linker.ld

SRC_C = $(wildcard $(SRC_DIR)/*.c)
SRC_S = $(wildcard $(SRC_DIR)/*.S)

OBJ_C = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRC_C))
OBJ_S = $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(SRC_S))

OBJ   = $(OBJ_C) $(OBJ_S)

ELF    = $(BUILD_DIR)/kernel.elf
KERNEL = $(BUILD_DIR)/kernel8.img

all: $(KERNEL)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(ELF): $(OBJ) | $(BUILD_DIR)
	$(LD) $(LDFLAGS) -o $@ $^

$(KERNEL): $(ELF)
	$(OBJCOPY) $< -O binary $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

# SD card image for BASIC LOAD/SAVE. Create a blank FAT16 disk with:
#   make sdcard
SDIMG = sd.img

$(SDIMG):
	dd if=/dev/zero of=$@ bs=1M count=64
	mkfs.fat -F 16 -n BERRYDISK $@

sdcard: $(SDIMG)

# Build a bootable Raspberry Pi 4 SD-card image (firmware + kernel + config.txt)
# that runs BerryBasic on real hardware. See tools/mksdimage.sh and README-realhw.md.
sdimage: $(KERNEL)
	tools/mksdimage.sh

run: $(KERNEL) $(SDIMG)
	qemu-system-aarch64 -M raspi4b -m 2G -kernel $(KERNEL) \
	    -serial stdio -display gtk -device usb-kbd \
	    -drive file=$(SDIMG),if=sd,format=raw

.PHONY: sdcard sdimage

# Native build of the interpreter for fast testing/debugging on the host.
# The interpreter (src/basic.c) is shared with the target; only the console
# backend differs.
HOSTCC      = cc
HOST_BIN    = $(BUILD_DIR)/basic_host
HOST_SRC    = $(SRC_DIR)/basic.c host/console_host.c host/storage_host.c host/main.c

host: $(HOST_SRC) | $(BUILD_DIR)
	$(HOSTCC) -Wall -g -I$(SRC_DIR) -o $(HOST_BIN) $(HOST_SRC)

.PHONY: all clean run host
