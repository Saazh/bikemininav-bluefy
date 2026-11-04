#!/usr/bin/env python3
"""Enable the Montserrat font sizes used by the larger round UI.
Run this AFTER bootstrap_drivers.py has generated firmware/include/lv_conf.h.
"""
from pathlib import Path
import re

conf = Path(__file__).resolve().parents[1] / "include" / "lv_conf.h"
if not conf.exists():
    raise SystemExit(f"Missing {conf}. Run python tools/bootstrap_drivers.py first.")
s = conf.read_text()
for font in (10, 12, 14, 20, 28, 48):
    key = f"LV_FONT_MONTSERRAT_{font}"
    s, count = re.subn(rf"(?m)^(\s*#define\s+{key}\s+)\d+", r"\g<1>1", s)
    if count != 1:
        raise SystemExit(f"Could not find exactly one {key} definition in lv_conf.h")
conf.write_text(s)
print("Enabled LVGL Montserrat 10, 12, 14, 20, 28, 48 in", conf)
