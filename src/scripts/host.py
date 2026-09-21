#!/usr/bin/env python3
# host.py — host side of the BioOS virtio-console channel.
# Usage: python3 scripts/host.py "ping"        send a line, print reply
#        python3 scripts/host.py -f file.bin   ingest raw bytes -> object
#        python3 scripts/host.py --vcf f.vcf   ingest VCF -> variants array
#        python3 scripts/host.py lineage <hex> pull lineage graph
# Socket path: build/<arch>/host.sock (created by QEMU's chardev).
# Override with BIOOS_SOCK.
import os
import socket
import sys
import time

SOCK = os.environ.get("BIOOS_SOCK", "build/aarch64/host.sock")


def connect() -> socket.socket:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    for _ in range(50):                       # QEMU may not be up yet
        try:
            s.connect(SOCK)
            return s
        except OSError:
            time.sleep(0.1)
    print("host.py: cannot connect to", SOCK, file=sys.stderr)
    sys.exit(1)


def reply(s: socket.socket, act: bool = False) -> int:
    s.settimeout(10)
    try:
        data = b""
        while True:
            chunk = s.recv(8192)
            if not chunk:
                break
            data += chunk
            if not act or data.endswith(b"\n.\n"):
                break
        if act and data.endswith(b"\n.\n"):
            data = data[:-2]
        print(data.decode(errors="replace"), end="")
        return 0 if data.startswith(b"ok") or act else 1
    except socket.timeout:
        print("host.py: no reply", file=sys.stderr)
        return 1


def main() -> int:
    args = sys.argv[1:]
    if not args:
        args = ["ping"]

    s = connect()
    if args[0] == "-f":
        path = args[1]
        data = open(path, "rb").read()
        name = os.path.basename(path)
        s.sendall(f"putfile {name} {len(data)}\n".encode() + data)
        return reply(s)
    if args[0] == "--vcf":
        data = open(args[1], "rb").read()
        s.sendall(f"putvcf {len(data)}\n".encode() + data)
        return reply(s)

    s.sendall((" ".join(args)).encode() + b"\n")
    return reply(s, act=args[0] == "act")


if __name__ == "__main__":
    sys.exit(main())
