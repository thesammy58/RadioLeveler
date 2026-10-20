# SPDX-License-Identifier: MIT
# Copyright (c) 2026 thesammy58
# Written with Claude, Anthropic's AI model; see "How this was made" in README.md.
"""Packs the player download from the Visual Studio build: dist/RadioLeveler-v<version>.zip

    python tools/make_release.py [path\\to\\RadioLeveler.asi]   (default: Release\\RadioLeveler.asi)

The zip holds only the two folders the files go into:
    Game/Bin/RadioLeveler.asi and RadioLeveler.ini  -> The Sims 3/Game/Bin/
    Mods/Packages/RadioLeveler.package              -> Documents/Electronic Arts/The Sims 3/Mods/Packages/"""
import hashlib, os, re, sys, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    asi = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "Release", "RadioLeveler.asi")
    src = open(os.path.join(ROOT, "src", "radio_leveler.cpp"), encoding="utf-8").read()
    version = re.search(r'#define RL_VERSION "([^"]+)"', src).group(1)
    files = [
        ("Game/Bin/RadioLeveler.asi", asi),
        ("Game/Bin/RadioLeveler.ini", os.path.join(ROOT, "RadioLeveler.ini")),
        ("Mods/Packages/RadioLeveler.package", os.path.join(ROOT, "package", "RadioLeveler.package")),
    ]
    for _, path in files:
        if not os.path.exists(path):
            sys.exit(f"missing {path}: build RadioLeveler.slnx (Release | x86) first")
    ini = open(files[1][1], "rb").read()
    if b"\r\n" not in ini:
        ini = ini.replace(b"\n", b"\r\n")   # Notepad-friendly line endings whatever the checkout did
    os.makedirs(os.path.join(ROOT, "dist"), exist_ok=True)
    out = os.path.join(ROOT, "dist", f"RadioLeveler-v{version}.zip")
    with zipfile.ZipFile(out, "w") as z:
        for name, path in files:
            data = ini if name.endswith(".ini") else open(path, "rb").read()
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, data, compresslevel=9)
    print(f"wrote {out}")
    print(f"RadioLeveler.asi SHA-256: {hashlib.sha256(open(asi, 'rb').read()).hexdigest()}")


if __name__ == "__main__":
    main()
