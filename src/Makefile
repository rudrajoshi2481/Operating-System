CC      := clang
LD      := ld.lld
QEMU    := qemu-system-aarch64
EDK2    := /opt/homebrew/share/qemu/edk2-aarch64-code.fd
LIMINE  := ../limine

BUILD   := build
KERNEL  := $(BUILD)/kernel.elf
IMG     := $(BUILD)/bioos.img

CFLAGS  := --target=aarch64-none-elf -ffreestanding -nostdlib \
           -Wall -Wextra -Werror -mgeneral-regs-only \
           -fno-stack-protector -fno-stack-check -fno-PIC \
           -O2 -g -I. -Ikernel -Iarch/aarch64

LDFLAGS := -nostdlib -static -z max-page-size=4096 -T linker.ld

C_SRCS   := kernel/main.c kernel/kprint.c kernel/panic.c \
            arch/aarch64/uart.c arch/aarch64/mmio.c
ASM_SRCS := arch/aarch64/boot.S arch/aarch64/vectors.S
OBJS     := $(C_SRCS:%.c=$(BUILD)/%.o) $(ASM_SRCS:%.S=$(BUILD)/%.o)

QFLAGS  := -M virt -accel hvf -cpu host -m 256M \
           -bios $(EDK2) \
           -drive if=none,id=esp,format=raw,file=$(IMG) \
           -device virtio-blk-device,drive=esp \
           -display none

all: $(KERNEL)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(IMG): $(KERNEL) limine.conf
	sh scripts/mkimg.sh $(IMG) $(KERNEL) $(LIMINE)

run: $(IMG)
	$(QEMU) $(QFLAGS) -serial stdio

test: $(IMG)
	@QEMU="$(QEMU)" QFLAGS="$(QFLAGS)" sh scripts/smoke.sh

clean:
	rm -rf $(BUILD)

.PHONY: all run test clean
