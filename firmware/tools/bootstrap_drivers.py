#!/usr/bin/env python3
"""Copy the Waveshare LVGL9 demo's already-working board drivers.

Requires git and internet on YOUR computer; the upstream driver files are
not redistributed inside this example. Re-run with --source DIR if you've
already downloaded that source repository.
"""
from __future__ import annotations
import argparse
import pathlib
import re
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
DRIVER_FILES = (
    "amoled.cpp", "amoled.h", "board_config.h", "FT3168.cpp", "FT3168.h",
    "i2c.c", "i2c.h", "low_level_amoled.c", "low_level_amoled.h",
)
REPO = "https://github.com/thelastoutpostworkshop/waveshare_esp32s3_1.43_amoled_lvgl9.git"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, help="Existing cloned Waveshare demo directory")
    args = parser.parse_args()
    source = args.source
    if source is None:
        source = ROOT / ".waveshare_upstream"
        if not source.exists():
            subprocess.run(["git", "clone", "--depth", "1", REPO, str(source)], check=True)
    source = source.resolve()
    for filename in DRIVER_FILES:
        if not (source / filename).exists():
            raise FileNotFoundError(f"Missing upstream driver: {source / filename}")
        shutil.copyfile(source / filename, ROOT / "src" / filename)
    conf = (source / "lv_conf.h").read_text()
    # Use the exact LVGL 9 template version from upstream. Ensure the small
    # attribution text and debugging fonts are available.
    conf = re.sub(r"(?m)^#define LV_FONT_MONTSERRAT_10\s+\d+", "#define LV_FONT_MONTSERRAT_10 1", conf)
    (ROOT / "include").mkdir(exist_ok=True)
    (ROOT / "include" / "lv_conf.h").write_text(conf)
    print("Copied Waveshare LVGL9 drivers into firmware/src and lv_conf.h into include/")


if __name__ == "__main__":
    main()
