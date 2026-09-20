CC      := clang
LD      := ld.lld
QEMU    := qemu-system-aarch64
EDK2    := /opt/homebrew/share/qemu/edk2-aarch64-code.fd
LIMINE  := ../limine

BUILD   := build
KERNEL  := $(BUILD)/kernel.elf
IMG     := $(BUILD)/bioos.img
STORE   := $(BUILD)/store.img

CFLAGS  := --target=aarch64-none-elf -ffreestanding -nostdlib \
           -Wall -Wextra -Werror -mgeneral-regs-only \
           -fno-stack-protector -fno-stack-check -fno-PIC \
           -O2 -g -I. -Ikernel -Iarch/aarch64 -Idrivers

LDFLAGS := -nostdlib -static -z max-page-size=4096 -T linker.ld

C_SRCS   := kernel/main.c kernel/kprint.c kernel/panic.c kernel/exception.c \
            kernel/lib.c kernel/pmm.c kernel/heap.c kernel/thread.c \
            kernel/proc.c kernel/syscall.c kernel/shell.c \
            kernel/sha256.c kernel/objstore.c kernel/uri.c \
            drivers/virtio_mmio.c drivers/virtio_blk.c \
            drivers/virtio_console.c kernel/hostlink.c \
            arch/aarch64/uart.c arch/aarch64/mmio.c arch/aarch64/gic.c \
            arch/aarch64/irq.c arch/aarch64/timer.c arch/aarch64/vmm.c
ASM_SRCS := arch/aarch64/boot.S arch/aarch64/vectors.S arch/aarch64/switch.S \
            kernel/userblob.S

USER_OBJS := $(BUILD)/user/hello.o $(BUILD)/user/usys.o
OBJS     := $(C_SRCS:%.c=$(BUILD)/%.o) $(ASM_SRCS:%.S=$(BUILD)/%.o)

QFLAGS  := -M virt,gic-version=3 -accel hvf -cpu host -m 256M \
           -bios $(EDK2) \
           -drive if=none,id=esp,format=raw,file=$(IMG) \
           -device virtio-blk-device,drive=esp \
           -drive if=none,id=store,format=raw,file=$(STORE) \
           -device virtio-blk-device,drive=store \
           -device virtio-serial-device \
           -chardev socket,id=hchan,path=$(BUILD)/host.sock,server=on,wait=off \
           -device virtconsole,chardev=hchan

all: $(KERNEL)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/user/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/user/%.o: user/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/user/hello.elf: $(USER_OBJS) user/link.ld
	$(LD) -nostdlib -static -z max-page-size=4096 -T user/link.ld $(USER_OBJS) -o $@

$(BUILD)/kernel/userblob.o: $(BUILD)/user/hello.elf

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(IMG): $(KERNEL) limine.conf
	sh scripts/mkimg.sh $(IMG) $(KERNEL) $(LIMINE)

$(STORE):
	dd if=/dev/zero of=$@ bs=1m count=64

run: $(IMG) $(STORE)
	$(QEMU) $(QFLAGS) -display none -serial stdio

run-ui: $(IMG) $(STORE)
	$(QEMU) $(QFLAGS) -device virtio-gpu-device -display cocoa -serial stdio

test: $(IMG) $(STORE)
	@QEMU="$(QEMU)" QFLAGS="$(QFLAGS)" sh scripts/smoke.sh

clean:
	rm -rf $(BUILD)

# Host-side unit tests (C survival kit): allocator logic compiled for macOS
# with ASan/UBSan. Same .c files the kernel links.
HOSTCC   := clang
HOSTFLAGS := -fsanitize=address,undefined -g -O1 -Wall -Wextra -Werror \
             -I. -Ikernel -Itest
HOSTSRC  := kernel/pmm.c kernel/heap.c kernel/lib.c

check:
	$(HOSTCC) $(HOSTFLAGS) test/pmm_test.c  kernel/pmm.c kernel/lib.c test/stub.c -o $(BUILD)/pmm_test
	$(HOSTCC) $(HOSTFLAGS) test/heap_test.c $(HOSTSRC) test/stub.c -o $(BUILD)/heap_test
	$(HOSTCC) $(HOSTFLAGS) test/sha_test.c  kernel/sha256.c -o $(BUILD)/sha_test
	./$(BUILD)/pmm_test
	./$(BUILD)/heap_test
	./$(BUILD)/sha_test

.PHONY: all run run-ui test check clean
