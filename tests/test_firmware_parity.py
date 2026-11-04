"""Offline static checks to keep web/firmware protocol versions in sync."""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]

class ParityTests(unittest.TestCase):
    def test_protocol_shared_ids_and_opcodes(self):
        cpp=(ROOT/'firmware/src/main.cpp').read_text().lower()
        js=(ROOT/'web/protocol.mjs').read_text().lower()
        for uuid in ['7e9d1000-2a39-4b25-b3e4-cb7d1fc2ac10',
                     '7e9d1001-2a39-4b25-b3e4-cb7d1fc2ac10']:
            self.assertIn(uuid, cpp)
            self.assertIn(uuid, js)
        for opcode in ['0x02','0x03','0x04','0x05']:
            self.assertIn(opcode, cpp)
            self.assertIn(opcode, js)
    def test_map_dimensions_and_image_limit(self):
        cpp=(ROOT/'firmware/src/main.cpp').read_text()
        self.assertIn('MAP = 288',cpp)
        self.assertIn('MAX_JPG = 90000',cpp)

if __name__=='__main__': unittest.main()
