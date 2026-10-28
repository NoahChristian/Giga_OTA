#!/usr/bin/env python3
"""Simulate a real touchscreen tap, so a PC script can drive the UI.

Sends 'P' + 3-digit x + 3-digit y (zero-padded, e.g. "P402242" for
x=402,y=242) over Serial, which drives a second LVGL pointer indev
(see simTouchReadCb() in Giga_OTA.ino) through a real press-then-release
cycle -- actual LVGL hit-testing and click-bubbling, the same code path a
physical finger drives, not a shortcut around it.

Coordinates are in the sketch's logical 800x480 landscape space (the same
space dump_screen.py's PNGs are in), not raw panel pixels.

In this repo it is how the Update button gets pressed without hands on the
board, which makes the whole OTA cycle scriptable.

Usage:
    python sim_tap.py <x> <y> [COMx]        # COMx e.g. COM3; default COM9
"""
import sys
import time
import serial


def tap(ser, x, y):
    if not (0 <= x <= 799 and 0 <= y <= 479):
        raise ValueError(f"({x},{y}) is outside the 800x480 logical screen")
    cmd = b"P" + f"{x:03d}{y:03d}".encode("ascii")
    ser.write(cmd)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    x, y = int(sys.argv[1]), int(sys.argv[2])
    port = sys.argv[3] if len(sys.argv) > 3 else "COM9"

    ser = serial.Serial(port, 115200, timeout=10)
    tap(ser, x, y)
    time.sleep(0.2)  # let the ~80ms sim press-release + lv_timer_handler redraw settle
    print(f"Tapped ({x},{y}) on {port}")


if __name__ == "__main__":
    main()
