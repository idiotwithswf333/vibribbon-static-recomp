#!/usr/bin/env python3
"""Extract the data track of a PS1 BIN/CUE image into a folder.

Usage: python tools/extract_disc.py path/to/game.cue [out_dir]

You must supply your own legally obtained disc image. Nothing extracted
here should ever be committed (assets/ is gitignored).
"""
import hashlib
import json
import re
import struct
import sys
from pathlib import Path


def parse_cue(cue_path: Path):
    text = cue_path.read_text(errors="replace")
    m_file = re.search(r'FILE\s+"([^"]+)"', text)
    m_track = re.search(r"TRACK\s+01\s+(\S+)", text)
    if not m_file or not m_track:
        sys.exit("Could not find FILE / TRACK 01 in cue sheet")
    mode = m_track.group(1)
    # (sector size, offset of 2048-byte user data inside the sector)
    layouts = {
        "MODE2/2352": (2352, 24),
        "MODE1/2352": (2352, 16),
        "MODE1/2048": (2048, 0),
        "MODE2/2336": (2336, 8),
    }
    if mode not in layouts:
        sys.exit(f"Unsupported track mode {mode}")
    return cue_path.parent / m_file.group(1), layouts[mode]


class Disc:
    def __init__(self, bin_path, layout):
        self.f = open(bin_path, "rb")
        self.ss, self.off = layout

    def read(self, lba, size):
        out = bytearray()
        for i in range((size + 2047) // 2048):
            self.f.seek((lba + i) * self.ss + self.off)
            out += self.f.read(2048)
        return bytes(out[:size])


def walk(disc, lba, size, prefix=""):
    data = disc.read(lba, size)
    i = 0
    while i < len(data):
        n = data[i]
        if n == 0:  # padding to next sector
            i = (i // 2048 + 1) * 2048
            continue
        ext = struct.unpack("<I", data[i + 2 : i + 6])[0]
        length = struct.unpack("<I", data[i + 10 : i + 14])[0]
        is_dir = bool(data[i + 25] & 2)
        name_len = data[i + 32]
        name = data[i + 33 : i + 33 + name_len].decode("latin1")
        i += n
        if name in ("\x00", "\x01"):
            continue
        name = name.split(";")[0]
        if is_dir:
            yield from walk(disc, ext, length, prefix + name + "/")
        else:
            yield prefix + name, ext, length


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cue = Path(sys.argv[1])
    out = Path(sys.argv[2] if len(sys.argv) > 2 else "assets/disc")
    bin_path, layout = parse_cue(cue)
    disc = Disc(bin_path, layout)
    pvd = disc.read(16, 2048)
    if pvd[1:6] != b"CD001":
        sys.exit("No ISO9660 primary volume descriptor found")
    root_lba, root_len = struct.unpack("<I", pvd[158:162])[0], struct.unpack("<I", pvd[166:170])[0]

    manifest = {}
    for name, lba, size in walk(disc, root_lba, root_len):
        data = disc.read(lba, size)
        dest = out / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        manifest[name] = {"lba": lba, "size": size, "sha1": hashlib.sha1(data).hexdigest()}
        print(f"{name:40s} {size:>9d} bytes")
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2))
    print(f"\nExtracted {len(manifest)} files to {out}/")


if __name__ == "__main__":
    main()
