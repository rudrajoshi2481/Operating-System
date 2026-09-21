#!/bin/sh
# mkimg.sh <img> <kernel.elf> <limine-dir> [bootefi]
# bootefi defaults to BOOTAA64.EFI; pass BOOTX64.EFI for x86_64.
# Builds a 64 MiB FAT32 ESP image containing Limine, limine.conf, kernel.elf.
set -e

IMG="$1"
KERNEL="$2"
LIMINE="$3"
MNT=$(mktemp -d /tmp/bioos-esp.XXXXXX)
DEV=""
MDEV=""

cleanup() {
    [ -n "$MDEV" ] && hdiutil detach "$MDEV" -force >/dev/null 2>&1
    [ -n "$DEV" ] && hdiutil detach "$DEV" -force >/dev/null 2>&1
    rmdir "$MNT" 2>/dev/null
}
trap cleanup EXIT

# Drop attachments left behind by earlier failed runs of this script.
for d in $(hdiutil info | awk -v img="$IMG" \
        '$0 ~ "image-path.*" img {found=1} found && /^\/dev\// {print $1; found=0}'); do
    hdiutil detach "$d" -force >/dev/null 2>&1 || true
done

dd if=/dev/zero of="$IMG" bs=1m count=64 2>/dev/null

DEV=$(hdiutil attach -nomount "$IMG" | awk 'NR==1 {print $1}')
newfs_msdos -F 32 -v BIOOS "$DEV" >/dev/null
hdiutil detach "$DEV" >/dev/null
DEV=""

MDEV=$(hdiutil attach -mountpoint "$MNT" "$IMG" | awk '/\/dev\// {print $1; exit}')

mkdir -p "$MNT/EFI/BOOT"
cp "$LIMINE/${4:-BOOTAA64.EFI}" "$MNT/EFI/BOOT/"
cp limine.conf "$MNT/"
cp "$KERNEL" "$MNT/kernel.elf"

hdiutil detach "$MDEV" >/dev/null
MDEV=""
rmdir "$MNT"
trap - EXIT
