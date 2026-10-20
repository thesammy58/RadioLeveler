#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 thesammy58
# Written with Claude, Anthropic's AI model; see "How this was made" in README.md.
"""Runs a folder of songs through the offline harness and prints how even their loudness is
before and after leveling, for each Strength preset.

    cc -O2 -o obj/harness tests/harness.c -lm
    python3 tests/corpus.py path/to/songs [--harness obj/harness] [--skip] [--gap 0]

Every file ffmpeg can decode is used (mp3, ogg, wav, flac, ...). EA's radio songs are .snr/.sns
pairs: decode them to .wav first, e.g. with vgmstream-cli. Songs are played back to back, the
way the radio does, and song changes are signalled to the leveler exactly as the in-game
player tracking does. --skip cuts every song after a random 20-60 s, like pressing Skip."""
import argparse, os, random, statistics, struct, subprocess, sys, tempfile

PRESETS = {
    "off": ["--bypass", "1"],
    "standard": [],
    "relaxed": ["--priorUpSec", "3", "--priorDownSec", "1", "--upDbps", "4", "--downDbps", "20", "--forgetSec", "20"],
    "light": ["--priorUpSec", "10", "--priorDownSec", "2", "--upDbps", "1.5", "--downDbps", "6", "--forgetSec", "60"],
    "subtle": ["--priorUpSec", "15", "--priorDownSec", "2", "--upDbps", "1", "--downDbps", "6", "--forgetSec", "120"],
}
FS = 44100


def decode(path):
    return subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "f32le", "-ac", "2", "-ar", str(FS), "-"],
                          capture_output=True).stdout


def spread(v):
    v = sorted(v)
    n = len(v)
    return v[int(0.95 * (n - 1))] - v[int(0.05 * (n - 1))], v[-1] - v[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    ap.add_argument("--harness", default=os.path.join("obj", "harness"))
    ap.add_argument("--skip", action="store_true", help="cut each song after a random 20-60 s")
    ap.add_argument("--gap", type=float, default=0.0, help="seconds of silence between songs")
    ap.add_argument("--seed", type=int, default=3)
    a = ap.parse_args()

    files = sorted(os.path.join(r, f) for r, _, fs in os.walk(a.folder) for f in fs
                   if not f.lower().endswith((".txt", ".jpg", ".png", ".ini", ".sns")))
    if not files:
        sys.exit("no audio files found")
    rng = random.Random(a.seed)
    tmp = tempfile.mkdtemp()
    procs = {}
    for name, args in PRESETS.items():
        out = open(os.path.join(tmp, name + ".txt"), "w")
        procs[name] = subprocess.Popen([a.harness, "--player", "1"] + args, stdin=subprocess.PIPE,
                                       stdout=out, stderr=subprocess.DEVNULL)

    def send(blob):
        for p in procs.values():
            p.stdin.write(blob)

    used = []
    for i, f in enumerate(files):
        pcm = decode(f)
        frames = len(pcm) // 8
        if frames < FS * 10:
            continue
        if a.skip:
            frames = min(frames, int(rng.uniform(20, 60) * FS))
        send(b"SONG" + struct.pack("<ii", len(used), frames) + pcm[:frames * 8])
        if a.gap > 0:
            send(b"GAP " + struct.pack("<ii", -1, int(a.gap * FS)))
        used.append(f)
        print(f"\r{len(used)} songs", end="", file=sys.stderr, flush=True)
    print(file=sys.stderr)
    send(b"END " + struct.pack("<ii", 0, 0))
    for p in procs.values():
        p.stdin.close()
        p.wait()

    print(f"{len(used)} songs{', skipped after 20-60 s' if a.skip else ''}\n")
    print(f"{'':10} {'spread p5-p95':>14} {'loudest-quietest':>17} {'median LRA':>11} {'max peak':>9} {'gain sd':>8}")
    for name in PRESETS:
        rows = [list(map(float, l.split())) for l in open(os.path.join(tmp, name + ".txt")) if l.strip()]
        loud = [r[2] for r in rows]
        lra = statistics.median(r[6] for r in rows)
        peak = max(r[8] for r in rows)
        gsd = statistics.median(r[9] for r in rows) if name != "off" else 0.0
        p, rng_ = spread(loud)
        print(f"{name:10} {p:11.1f} dB {rng_:14.1f} dB {lra:8.1f} LU {peak:6.1f} dB {gsd:5.2f} dB")


if __name__ == "__main__":
    main()
