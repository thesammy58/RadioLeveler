# SPDX-License-Identifier: MIT
# Copyright (c) 2026 thesammy58
# Written with Claude, Anthropic's AI model; see "How this was made" in README.md.
"""RadioLeveler.ini (shipped in the release) must match kDefaultIni in src/radio_leveler.cpp
(what the ASI writes when the ini is missing).

    python tools/sync_ini.py           check, exit code 1 if they differ
    python tools/sync_ini.py --write   regenerate RadioLeveler.ini from the source"""
import ast, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def default_ini():
    src = open(os.path.join(ROOT, "src", "radio_leveler.cpp"), encoding="utf-8").read()
    body = re.search(r"const char kDefaultIni\[\] =\s*(.*?);\n", src, re.S).group(1)
    return "".join(ast.literal_eval(lit) for lit in re.findall(r'"(?:[^"\\]|\\.)*"', body)).encode("ascii")


def main():
    want = default_ini()
    path = os.path.join(ROOT, "RadioLeveler.ini")
    if "--write" in sys.argv:
        open(path, "wb").write(want)
        print("wrote", path)
        return
    have = open(path, "rb").read() if os.path.exists(path) else b""
    if have != want:
        sys.exit("RadioLeveler.ini doesn't match kDefaultIni in src/radio_leveler.cpp; run: python tools/sync_ini.py --write")
    print("RadioLeveler.ini matches kDefaultIni")


if __name__ == "__main__":
    main()
