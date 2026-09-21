#!/bin/sh
# smoke.sh — boot the OS headless, expect BIOOS_OK on serial.
# QEMU, QFLAGS, BUILD and ARCH are passed in from the Makefile.
set -u

LOG=$BUILD/serial.log
rm -f "$LOG" "$BUILD/host.sock"

$QEMU $QFLAGS -display none -serial file:"$LOG" &
PID=$!
sleep 10
kill "$PID" 2>/dev/null
wait "$PID" 2>/dev/null

ok=1
for pat in BIOOS_OK "interrupts armed" "virtio-blk:" "virtio-console:" \
           "objstore: selftest ok"; do
    grep -q "$pat" "$LOG" || ok=0
done
# EL0 user processes only exist on aarch64 so far
if [ "${ARCH:-aarch64}" = "aarch64" ]; then
    grep -q "hello from EL0" "$LOG" || ok=0
fi

if [ "$ok" = 1 ]; then
    echo "smoke: PASS"
else
    echo "smoke: FAIL"
    tail -40 "$LOG"
    exit 1
fi
