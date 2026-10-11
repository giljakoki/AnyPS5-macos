import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import unfself


def program_header(kind, offset, size):
    return struct.pack("<IIQQQQQQ", kind, 5, offset, offset, offset, size, size, 0x4000)


def fake_signed(magic=b"\x4f\x15\x3d\x1d", flags=0x2804):
    text = bytes(range(16)) * 4
    data = b"DATA" * 8
    version = b"version!"
    headers = [program_header(1, 0x1000, len(text)), program_header(1, 0x2000, len(data)), program_header(unfself.SCE_VERSION, 0x3000, len(version))]
    elf = bytearray(64)
    elf[:4] = b"\x7fELF"
    struct.pack_into("<Q", elf, 0x20, 64)
    struct.pack_into("<Q", elf, 0x28, 0x9999)
    struct.pack_into("<HHHH", elf, 0x36, 56, len(headers), 64, 7)
    elf += b"".join(headers)
    entries = [(0x110004, 0, 0x20), (flags, 0, len(text)), (0x102804, 0, len(data))]
    body = 0x20 + len(entries) * 0x20 + len(elf)
    payload = [b"\0" * 0x20, text, data]
    table = b""
    offset = body
    for (props, _, size), block in zip(entries, payload):
        table += struct.pack("<QQQQ", props, offset, size, size)
        offset += len(block)
    header = magic + struct.pack("<BBBBIHHQHHI", 0, 1, 1, 0x12, 0x101, 0, 0, offset, len(entries), 0x22, 0)
    return header + table + bytes(elf) + b"".join(payload) + version, text, data, version


class UnfselfTests(unittest.TestCase):
    def test_segments_land_at_their_program_header_offsets(self):
        for magic in unfself.MAGICS:
            container, text, data, version = fake_signed(magic)
            elf = unfself.unpack(container)
            self.assertEqual(elf[:4], b"\x7fELF")
            self.assertEqual(elf[0x1000:0x1000 + len(text)], text)
            self.assertEqual(elf[0x2000:0x2000 + len(data)], data)
            self.assertEqual(elf[0x3000:0x3000 + len(version)], version)
            self.assertEqual(struct.unpack_from("<QHHH", elf, 0x28)[0], 0)
            self.assertEqual(struct.unpack_from("<HHH", elf, 0x3a), (0, 0, 0))

    def test_encrypted_or_compressed_segments_are_refused(self):
        for flag in (0x2, 0x8):
            container, *_ = fake_signed(flags=0x2804 | flag)
            with self.assertRaisesRegex(ValueError, "encrypted or compressed"):
                unfself.unpack(container)

    def test_an_elf_is_not_a_self(self):
        with self.assertRaisesRegex(ValueError, "not a SELF container"):
            unfself.unpack(b"\x7fELF" + bytes(60))


if __name__ == "__main__":
    unittest.main()
