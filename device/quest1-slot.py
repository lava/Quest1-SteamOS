#!/usr/bin/env python3
"""Show the bootloader's A/B slot flags, or mark the booted slot successful.

  quest1-slot status
  quest1-slot mark-successful [--dry-run]

The Quest bootloader keeps the slot state in bits 48-55 of the GPT attributes of boot_a and
boot_b (Qualcomm layout): priority (2 bits), active, retry count (3 bits), successful, unbootable.
It counts the retry count down on each boot of a slot that is not marked successful, and falls
back to the other slot at 0. Android's boot control HAL marks the slot; under Holo,
quest1-slot.service does. Only the attributes of one entry change; both GPT copies get new CRCs.
Installed as /usr/local/sbin/quest1-slot.
"""
import argparse
import contextlib
import fcntl
import os
import re
import stat
import struct
import zlib
from collections.abc import Iterator

BLKROSET = 0x125D
BLKROGET = 0x125E
SUCCESSFUL = 1 << 54
UNBOOTABLE = 1 << 55
SLOTS = ("_a", "_b")


def find_disk() -> tuple[str, int]:
    disks = {}
    for name in os.listdir("/sys/class/block"):
        try:
            with open(f"/sys/class/block/{name}/uevent") as f:
                uevent = f.read()
        except OSError:
            continue
        m = re.search(r"^PARTNAME=boot(_[ab])$", uevent, re.M)
        if m:
            disks[m.group(1)] = os.path.basename(os.path.dirname(os.path.realpath(f"/sys/class/block/{name}")))
    if sorted(disks) != list(SLOTS) or disks["_a"] != disks["_b"]:
        raise SystemExit(f"Expected boot_a and boot_b on one disk, found {disks}")
    with open(f"/sys/block/{disks['_a']}/queue/logical_block_size") as f:
        return f"/dev/{disks['_a']}", int(f.read())


def booted_slot() -> str:
    with open("/proc/cmdline") as f:
        m = re.search(r"(?:^|\s)androidboot\.slot_suffix=(_[ab])(?:\s|$)", f.read())
    if not m:
        raise SystemExit("No androidboot.slot_suffix on the kernel command line")
    return m.group(1)


class Copy:
    """One GPT copy: header sector and entry array."""

    def __init__(self, fd: int, sector: int, lba: int):
        self.fd, self.sector, self.lba = fd, sector, lba
        self.header = bytearray(os.pread(fd, sector, lba * sector))
        if self.header[:8] != b"EFI PART":
            raise SystemExit(f"No GPT header at LBA {lba}")
        self.size = struct.unpack_from("<I", self.header, 12)[0]
        crc = struct.unpack_from("<I", self.header, 16)[0]
        if crc != self.header_crc():
            raise SystemExit(f"Bad GPT header CRC at LBA {lba}")
        self.alternate, = struct.unpack_from("<Q", self.header, 32)
        self.entries_lba, count, self.entry_size, entries_crc = struct.unpack_from("<QIII", self.header, 72)
        length = count * self.entry_size
        self.entries = bytearray(os.pread(fd, length, self.entries_lba * sector))
        if len(self.entries) != length or zlib.crc32(self.entries) != entries_crc:
            raise SystemExit(f"Bad GPT entry array CRC for the header at LBA {lba}")

    def header_crc(self) -> int:
        h = bytearray(self.header[:self.size])
        h[16:20] = bytes(4)
        return zlib.crc32(h)

    def find(self, name: str) -> int:
        for offset in range(0, len(self.entries), self.entry_size):
            if self.entries[offset + 56:offset + 128].decode("utf-16le").rstrip("\0") == name:
                return offset
        raise SystemExit(f"No partition {name}")

    def attributes(self, name: str) -> int:
        value: int = struct.unpack_from("<Q", self.entries, self.find(name) + 48)[0]
        return value

    def set_attributes(self, name: str, value: int) -> None:
        struct.pack_into("<Q", self.entries, self.find(name) + 48, value)
        struct.pack_into("<I", self.header, 88, zlib.crc32(self.entries))
        struct.pack_into("<I", self.header, 16, self.header_crc())

    def write(self) -> None:
        os.pwrite(self.fd, self.entries, self.entries_lba * self.sector)
        os.pwrite(self.fd, self.header, self.lba * self.sector)


def describe(attributes: int) -> str:
    a = attributes >> 48
    return (f"priority={a & 3} active={a >> 2 & 1} retry={a >> 3 & 7} "
            f"successful={a >> 6 & 1} unbootable={a >> 7 & 1}")


@contextlib.contextmanager
def writable(fd: int) -> Iterator[None]:
    """quest1-modem sets every Android disk read-only in the kernel; lift that for this write only."""
    if not stat.S_ISBLK(os.fstat(fd).st_mode):
        yield
        return
    ro = struct.unpack("i", fcntl.ioctl(fd, BLKROGET, bytes(4)))[0]
    if ro:
        fcntl.ioctl(fd, BLKROSET, struct.pack("i", 0))
    try:
        yield
    finally:
        if ro:
            fcntl.ioctl(fd, BLKROSET, struct.pack("i", 1))


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("command", choices=("status", "mark-successful"))
    p.add_argument("--dry-run", action="store_true", help="show the change without writing it")
    # For tests against a disk image
    p.add_argument("--disk", help=argparse.SUPPRESS)
    p.add_argument("--sector-size", type=int, default=4096, help=argparse.SUPPRESS)
    p.add_argument("--slot", choices=SLOTS, help=argparse.SUPPRESS)
    a = p.parse_args()
    disk, sector = (a.disk, a.sector_size) if a.disk else find_disk()
    slot = a.slot or booted_slot()
    write = a.command == "mark-successful" and not a.dry_run
    fd = os.open(disk, os.O_RDWR if write else os.O_RDONLY)
    try:
        primary = Copy(fd, sector, 1)
        backup = Copy(fd, sector, primary.alternate)
        if primary.entries != backup.entries:
            raise SystemExit("Primary and backup GPT entries differ; not touching them")
        current = {s: primary.attributes(f"boot{s}") for s in SLOTS}
        if a.command == "status":
            print(f"booted: {slot}")
            for s in SLOTS:
                print(f"boot{s}: {describe(current[s])}")
            return
        if not current[slot] >> 50 & 1:
            print(f"boot{slot} is not the active slot; nothing to mark")
            return
        new = (current[slot] | SUCCESSFUL) & ~UNBOOTABLE
        if new == current[slot]:
            print(f"boot{slot} already marked successful")
            return
        print(f"boot{slot}: {describe(current[slot])} -> {describe(new)}")
        if a.dry_run:
            return
        for copy in (backup, primary):  # the bootloader reads the primary copy: it changes last
            copy.set_attributes(f"boot{slot}", new)
        with writable(fd):
            backup.write()
            os.fsync(fd)
            primary.write()
            os.fsync(fd)
        if Copy(fd, sector, 1).attributes(f"boot{slot}") != new:
            raise SystemExit("Read-back of the primary GPT does not show the change")
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
