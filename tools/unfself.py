"""Unpacks a fake-signed PS4/PS5 SELF, whose segments are stored unencrypted and uncompressed, into the
plain ELF the relinker reads, such as the eboot.bin of a homebrew release.

    python3 tools/unfself.py <input SELF> <output ELF>

The SELF header is followed by its segment entries and the ELF header and program headers. Each entry
with blocks holds the data of the program header its id names; the SCE version segment is the tail of
the file. Encrypted or compressed entries (a retail title) are refused. The SELF has no section
headers, so the ELF has none either."""
import struct
import sys
from pathlib import Path

MAGICS = (bytes.fromhex("4f153d1d"), bytes.fromhex("5414f5ee"))
SCE_VERSION = 0x6fffff01


def unpack(data, name="input"):
    if data[:4] not in MAGICS:
        raise ValueError(f"{name}: not a SELF container (magic {data[:4].hex()})")
    entries, = struct.unpack_from("<H", data, 0x18)
    elf = 0x20 + entries * 0x20
    if data[elf:elf + 4] != b"\x7fELF":
        raise ValueError(f"{name}: no ELF header after the SELF entries")
    phoff, = struct.unpack_from("<Q", data, elf + 0x20)
    phnum, = struct.unpack_from("<H", data, elf + 0x38)
    table = phoff + phnum * 56
    headers = [struct.unpack_from("<IIQQQQQQ", data, elf + phoff + index * 56) for index in range(phnum)]
    out = bytearray(max([table] + [header[2] + header[5] for header in headers]))
    out[:table] = data[elf:elf + table]
    struct.pack_into("<Q", out, 0x28, 0)
    struct.pack_into("<HHH", out, 0x3a, 0, 0, 0)
    written = set()
    for index in range(entries):
        props, offset, size, _ = struct.unpack_from("<QQQQ", data, 0x20 + index * 0x20)
        if (props >> 1) & 1 or (props >> 3) & 1:
            raise ValueError(f"{name}: segment entry {index} is encrypted or compressed; only a fake-signed SELF can be unpacked")
        if not (props >> 11) & 1:
            continue
        segment = (props >> 20) & 0xfff
        if segment >= phnum:
            raise ValueError(f"{name}: segment entry {index} names program header {segment} of {phnum}")
        start, length = headers[segment][2], headers[segment][5]
        if size != length or offset + size > len(data):
            raise ValueError(f"{name}: segment entry {index} holds {size:#x} bytes, program header {segment} {length:#x}")
        out[start:start + length] = data[offset:offset + size]
        written.add(segment)
    for index, header in enumerate(headers):
        if header[0] == SCE_VERSION and index not in written and 0 < header[5] <= len(data):
            out[header[2]:header[2] + header[5]] = data[len(data) - header[5]:]
            written.add(index)
    missing = [index for index, header in enumerate(headers) if header[0] == 1 and header[5] != 0 and index not in written]
    if missing:
        raise ValueError(f"{name}: loadable program headers {missing} have no data in the SELF")
    return bytes(out)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: unfself.py <input SELF> <output ELF>")
    source, target = Path(sys.argv[1]), Path(sys.argv[2])
    try:
        elf = unpack(source.read_bytes(), str(source))
    except ValueError as error:
        raise SystemExit(f"error: {error}")
    target.write_bytes(elf)
    print(f"{target}: ELF, {len(elf)} bytes")


if __name__ == "__main__":
    main()
