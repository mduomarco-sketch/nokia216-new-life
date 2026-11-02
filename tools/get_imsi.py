#!/usr/bin/env python3
"""Read the SIM IMSI from an Android phone over ADB (USB debugging on).

`service call iphonesubinfo N` returns a raw Parcel; the method number for
getSubscriberId shifts between Android versions/OEMs, so try a range and
keep whatever decodes to a 14-15 digit number.
"""
import os
import re
import subprocess

# adb from ../toolchain/platform-tools if it's there, else whatever is on PATH
_local = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "toolchain", "platform-tools", "adb.exe")
ADB = _local if os.path.exists(_local) else "adb"


def adb(*args):
    return subprocess.run([ADB, *args], capture_output=True, text=True).stdout


def parcel_string(out):
    words = re.findall(r"\b([0-9a-f]{8})\b", out.split("Parcel(", 1)[-1])
    raw = b"".join(int(w, 16).to_bytes(4, "little") for w in words)
    # word 0 = status, word 1 = string length, then UTF-16LE chars
    return raw[8:].decode("utf-16-le", "ignore").split("\x00")[0]


def main():
    print(adb("devices").strip())
    found = set()
    for code in range(1, 20):
        for extra in ([], ["s16", "com.android.shell"]):
            s = parcel_string(adb("shell", "service", "call", "iphonesubinfo", str(code), *extra))
            if re.fullmatch(r"\d{14,15}", s):
                found.add((code, s))
    # IMEIs are 15 digits too; the IMSI starts with the network code (214 = Spain).
    for code, s in sorted(found):
        tag = "  <-- IMSI (Spain)" if s.startswith("214") else ""
        print(f"code {code:2d}: {s}{tag}")
    if not found:
        print("Nothing readable - try:  adb shell content query --uri content://telephony/siminfo --projection imsi:display_name")


if __name__ == "__main__":
    main()
