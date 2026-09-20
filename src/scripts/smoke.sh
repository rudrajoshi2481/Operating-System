#!/bin/sh
# smoke.sh — boot the OS headless, expect BIOOS_OK on serial.
# QEMU and QFLAGS are passed in from the Makefile.
set -u

LOG=build/serial.log
rm -f "$LOG" build/host.sock

$QEMU $QFLAGS -display none -serial file:"$LOG" &
PID=$!
sleep 10
kill "$PID" 2>/dev/null
wait "$PID" 2>/dev/null

if grep -q BIOOS_OK "$LOG" && grep -q "tick" "$LOG" \
        && grep -q "hello from EL0" "$LOG" \
        && grep -q "virtio-blk:" "$LOG" \
        && grep -q "virtio-console:" "$LOG"; then
    echo "smoke: PASS"
else
    echo "smoke: FAIL"
    tail -40 "$LOG"
    exit 1
fi
