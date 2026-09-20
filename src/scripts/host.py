#!/usr/bin/env python3
# host.py — host side of the BioOS virtio-console channel.
# Usage: python3 scripts/host.py "ping"      (send a line, print reply)
#        python3 scripts/host.py -f file.bin (send file bytes)
# Socket path: build/host.sock (created by QEMU's chardev).
import socket
import sys
import time

SOCK = "build/host.sock"


def main() -> int:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    for _ in range(50):                       # QEMU may not be up yet
        try:
            s.connect(SOCK)
            break
        except OSError:
            time.sleep(0.1)
    else:
        print("host.py: cannot connect to", SOCK, file=sys.stderr)
        return 1

    s.settimeout(5)
    if len(sys.argv) > 1 and sys.argv[1] == "-f":
        with open(sys.argv[2], "rb") as f:
            s.sendall(f.read())
        print("sent", sys.argv[2])
        return 0

    msg = " ".join(sys.argv[1:]) or "ping"
    s.sendall(msg.encode() + b"\n")
    try:
        data = s.recv(4096)
        print(data.decode(errors="replace"), end="")
    except socket.timeout:
        print("host.py: no reply", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
