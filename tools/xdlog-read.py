#!/usr/bin/env python3
"""Print the boots saved by kernel/xdlog.c from a dump of the logdump partition.

    dd if=/dev/block/by-name/logdump of=/sdcard/logdump.img bs=1M
    xdlog-read.py logdump.img            # list the boots
    xdlog-read.py logdump.img 12         # print boot 12
"""
import struct
import sys

PAGE = 4096
MAGIC = 0x31474F4C44585858
HEAD = 1024 * 1024


def text(buf: bytes) -> bytes:
    """Bytes up to the first NUL: anything after it is older data."""
    end = buf.find(b"\0")
    return buf if end < 0 else buf[:end]


def main() -> int:
    data = open(sys.argv[1], "rb").read()
    magic, last, slots, slot_bytes = struct.unpack_from("<4Q", data)
    if magic != MAGIC:
        print("no xdlog header: this kernel has not written here")
        return 1

    boots = {}
    for boot in range(max(0, last - slots + 1), last + 1):
        start = PAGE + (boot % slots) * slot_bytes
        slot = data[start:start + slot_bytes]
        if slot.startswith(b"=== xdlog boot %d ===" % boot):
            boots[boot] = slot

    if len(sys.argv) < 3:
        for boot, slot in boots.items():
            head = text(slot[:HEAD])
            tail = head.rstrip().rsplit(b"\n", 1)[-1][:110].decode(errors="replace")
            mark = "  <- running when dumped" if boot == last else ""
            print(f"boot {boot}: {len(head)} bytes{mark}\n  last: {tail}")
        return 0

    slot = boots.get(int(sys.argv[2]))
    if slot is None:
        print("that boot is not saved")
        return 1
    out = sys.stdout.buffer
    head = slot[:HEAD]
    out.write(text(head))
    ring = slot[HEAD:]
    if len(text(head)) == HEAD and any(ring):
        # the ring wrapped at some point: older lines sit after the newest text
        new = text(ring)
        rest = ring[len(new):].lstrip(b"\0")
        out.write(b"\n=== lines lost here, older ring ===\n" + text(rest))
        out.write(b"\n=== newest ===\n" + new)
    return 0


if __name__ == "__main__":
    sys.exit(main())
