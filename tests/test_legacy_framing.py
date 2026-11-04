"""Basic local CRC32/framing sanity checks, no board or iPhone required."""
import struct
import unittest
import zlib


def crc32_firmware_style(data):
    crc = 0xffffffff
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xedb88320 if crc & 1 else 0)
    return (~crc) & 0xffffffff


def make_packets(jpeg, mtu=182):
    out = [b'\x02' + struct.pack('<II', len(jpeg), zlib.crc32(jpeg))]
    for i in range(0, len(jpeg), mtu-1):
        out.append(b'\x03' + jpeg[i:i+mtu-1])
    out.append(b'\x04')
    return out


class PacketTests(unittest.TestCase):
    def test_crc32_agrees_with_zlib(self):
        for data in [b'', b'abcdef', bytes(range(256)), b'\xff'*25000]:
            self.assertEqual(crc32_firmware_style(data), zlib.crc32(data))

    def test_length_and_endianness(self):
        jpeg = bytes(range(256))*100
        packets = make_packets(jpeg)
        self.assertEqual(packets[0][0], 2)
        size, checksum = struct.unpack('<II', packets[0][1:])
        self.assertEqual(size, len(jpeg))
        self.assertEqual(checksum, zlib.crc32(jpeg))
        self.assertEqual(b''.join(p[1:] for p in packets[1:-1]), jpeg)
        self.assertTrue(all(len(p) <= 182 for p in packets))
        self.assertEqual(packets[-1], b'\x04')


if __name__ == '__main__':
    unittest.main()
