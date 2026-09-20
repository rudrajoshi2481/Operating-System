#!/bin/sh
# mkimg.sh <img> <kernel.elf> <limine-dir>
# Builds a 64 MiB FAT32 ESP image containing Limine, limine.conf, kernel.elf.
set -e

IMG="$1"
KERNEL="$2"
LIMINE="$3"
MNT=$(mktemp -d /tmp/bioos-esp.XXXXXX)

dd if=/dev/zero of="$IMG" bs=1m count=64 2>/dev/null

DEV=$(hdiutil attach -nomount "$IMG" | awk 'NR==1 {print $1}')
newfs_msdos -F 32 -v BIOOS "$DEV" >/dev/null
hdiutil detach "$DEV" >/dev/null

MDEV=$(hdiutil attach -mountpoint "$MNT" "$IMG" | awk '/\/dev\// {print $1; exit}')
trap 'hdiutil detach "$MDEV" >/dev/null 2>&1; rmdir "$MNT" 2>/dev/null' EXIT

mkdir -p "$MNT/EFI/BOOT"
cp "$LIMINE/BOOTAA64.EFI" "$MNT/EFI/BOOT/"
cp limine.conf "$MNT/"
cp "$KERNEL" "$MNT/kernel.elf"

hdiutil detach "$MDEV" >/dev/null
rmdir "$MNT"
trap - EXIT
