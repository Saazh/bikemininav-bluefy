from pathlib import Path
import re
import unittest

HERE = Path(__file__).resolve().parents[1]
SOURCE = (HERE / "firmware/src/main.cpp").read_text()
WEB = Path('/mnt/data/BikeMiniNav_Apple_Map_Update/web/protocol.mjs')

class FirmwareContractTests(unittest.TestCase):
    def test_same_uuid_as_existing_web(self):
        if WEB.exists():
            web = WEB.read_text().upper()
            for uuid in ('7E9D1000-2A39-4B25-B3E4-CB7D1FC2AC10',
                         '7E9D1001-2A39-4B25-B3E4-CB7D1FC2AC10'):
                self.assertIn(uuid, SOURCE)
                self.assertIn(uuid, web)

    def test_advertising_name_remains_discoverable(self):
        self.assertIn('advertising->setName("BikeMiniNav");', SOURCE)
        self.assertNotIn('advertising->addServiceUUID(', SOURCE)

    def test_protocol_and_frame_unchanged(self):
        for fragment in ('IN_MAP = 288', 'MAX_JPG = 90000',
                         'case 0x01:', 'case 0x02:', 'case 0x03:',
                         'case 0x04:', 'case 0x05:', 'if (n != 12)',
                         'crc32(rxJPEG, rxCount) == expectedCRC'):
            self.assertIn(fragment, SOURCE)

    def test_three_pages_and_swipe(self):
        self.assertIn('MAP = 0, TURN = 1, RIDE = 2', SOURCE)
        self.assertIn('pendingGesture = dx < 0 ? +1 : -1;', SOURCE)
        self.assertIn('lv_timer_handler();', SOURCE)

    def test_jpeg_buffer_sizing(self):
        self.assertIn('SCREEN * SCREEN * sizeof(uint16_t)', SOURCE)
        self.assertIn('IN_MAP * IN_MAP * sizeof(uint16_t)', SOURCE)
        for x in range(466):
            src = x * (288-1) * 256 // (466-1)
            self.assertLessEqual(src >> 8, 287)

    def test_fonts_have_preflight(self):
        script = (HERE / 'firmware/tools/enable_apple_fonts.py').read_text()
        for n in (10, 12, 14, 20, 28, 48):
            self.assertIn(f'lv_font_montserrat_{n}', SOURCE)
        self.assertIn('(10, 12, 14, 20, 28, 48)', script)

if __name__ == '__main__':
    unittest.main()
