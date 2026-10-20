# SPDX-License-Identifier: MIT
# Copyright (c) 2026 thesammy58
# Written with Claude, Anthropic's AI model; see "How this was made" in README.md.
"""Packs the player download from build.sh's output: dist/RadioLeveler-v<version>.zip

    python tools/make_release.py

The zip holds only the two folders the files go into:
    Game/Bin/RadioLeveler.asi, .ini, .pdb   -> The Sims 3/Game/Bin/
    Mods/Packages/RadioLeveler.package      -> Documents/Electronic Arts/The Sims 3/Mods/Packages/
Entries get a fixed timestamp, so the same build always gives the same zip."""
import os, re, sys, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, "dist")


def main():
    src = open(os.path.join(ROOT, "src", "radio_leveler.cpp"), encoding="utf-8").read()
    version = re.search(r'#define RL_VERSION "([^"]+)"', src).group(1)
    files = [
        ("Game/Bin/RadioLeveler.asi", os.path.join(DIST, "RadioLeveler.asi")),
        ("Game/Bin/RadioLeveler.ini", os.path.join(DIST, "RadioLeveler.ini")),
        ("Game/Bin/RadioLeveler.pdb", os.path.join(DIST, "RadioLeveler.pdb")),
        ("Mods/Packages/RadioLeveler.package", os.path.join(ROOT, "package", "RadioLeveler.package")),
    ]
    for _, path in files:
        if not os.path.exists(path):
            sys.exit(f"missing {path}: run ./build.sh first")
    out = os.path.join(DIST, f"RadioLeveler-v{version}.zip")
    with zipfile.ZipFile(out, "w") as z:
        for name, path in files:
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, open(path, "rb").read(), compresslevel=9)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
