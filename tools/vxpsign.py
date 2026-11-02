#!/usr/bin/env python3
"""Sign a MediaTek MRE .vxp for Nokia Series 30+ phones (offline vxpatch equivalent).

S30+ only launches a .vxp whose tag table carries the IMSI of the inserted SIM.
Same scheme as vxpatch.luxferre.top / paulober/vxp-dev-sign:
  - tag 0x02 (app id) -> 0xFFFFFFFF
  - tag 0x12 (IMSI)   -> "9" + IMSI, ASCII, length field updated

Usage:
  python vxpsign.py info  app.vxp
  python vxpsign.py sign  app.vxp 214011234567890 [out.vxp]
"""
import struct
import sys

TAG_APPID = 0x02
TAG_IMSI = 0x12


def tags(data):
    """Yield (offset_of_value, tag_id, length) for each tag table entry."""
    i = struct.unpack_from("<I", data, len(data) - 12)[0]
    while i + 8 <= len(data):
        tag, length = struct.unpack_from("<II", data, i)
        if tag == 0:
            return
        yield i + 8, tag, length
        i += 8 + length


def sign(data, imsi):
    imsi = imsi.strip()
    if not imsi.isdigit() or not 14 <= len(imsi) <= 15:
        raise ValueError("IMSI should be 14-15 digits, e.g. 214011234567890")
    value = ("9" + imsi).encode("ascii")
    out = bytearray(data)
    seen_imsi = False
    # Walk the table on the original bytes; apply edits back-to-front so offsets stay valid.
    edits = []
    for off, tag, length in tags(data):
        if tag == TAG_APPID and length >= 4:
            edits.append((off, off + 4, b"\xff\xff\xff\xff", None))
        elif tag == TAG_IMSI:
            edits.append((off, off + length, value, off - 4))
            seen_imsi = True
    if not seen_imsi:
        raise ValueError("no IMSI tag (0x12) found - not an S30+-style vxp?")
    for start, end, new, len_off in sorted(edits, reverse=True):
        out[start:end] = new
        if len_off is not None:
            struct.pack_into("<I", out, len_off, len(new))
    return bytes(out)


def main(argv):
    if len(argv) < 3 or argv[1] not in ("info", "sign"):
        sys.exit(__doc__)
    data = open(argv[2], "rb").read()
    if argv[1] == "info":
        for off, tag, length in tags(data):
            raw = data[off:off + length]
            txt = raw.decode("ascii", "replace") if raw.isascii() and length < 64 else raw[:16].hex()
            print(f"tag 0x{tag:02x} len {length:4d} @0x{off:06x}: {txt}")
        return
    out_path = argv[4] if len(argv) > 4 else argv[2].replace(".vxp", "_signed.vxp")
    open(out_path, "wb").write(sign(data, argv[3]))
    print("signed ->", out_path)


if __name__ == "__main__":
    main(sys.argv)
