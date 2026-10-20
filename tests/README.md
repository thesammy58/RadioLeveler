# Tests

Everything here runs without the game. `leveler.h` is plain C, so the DSP tests build with any C compiler. The ABI test is a Windows program that also runs under Wine.

| File | What it checks | Build and run |
|---|---|---|
| `abi_test.cpp` | Loads the real `RadioLeveler.asi` into a fake game. Checks that it hooks the plug-in descriptor, that its output matches `leveler.h` bit for bit across three song changes, and that `Enabled=0` passes audio through untouched. | `./build.sh --tests`, then run `TS3.exe` from the `obj/test` folder (`wine TS3.exe` on Linux). It exits non-zero on failure. |
| `ab_test.c` | Checks that A/B bypass outputs the input delayed by the lookahead, bit for bit, and that switching doesn't click. | `cc -O2 -o obj/ab_test tests/ab_test.c -lm && obj/ab_test` (exit code 0 = pass) |
| `bench.c` | Measures the CPU cost of one stereo voice. | `cc -O2 -o obj/bench tests/bench.c -lm && obj/bench` |
| `harness.c` + `corpus.py` | Runs real songs through the leveler and measures BS.1770 loudness and LRA for each song, before and after. The measurements in the README and [docs/TECHNICAL.md](../docs/TECHNICAL.md) come from this. | See below. |

## Measuring with your own music

```sh
cc -O2 -o obj/harness tests/harness.c -lm
python3 tests/corpus.py "path/to/music" --harness obj/harness
python3 tests/corpus.py "path/to/music" --harness obj/harness --skip   # songs cut after 20-60 s
```

`corpus.py` needs `ffmpeg` on the PATH. It plays every song back to back through each preset and prints one table:

- **spread p5–p95:** how far apart the songs' loudness ends up, ignoring the 5 % most extreme at each end.
- **loudest−quietest:** the full range, outliers included.
- **median LRA:** how much loud/quiet contrast is left within songs.
- **max peak:** the highest output peak.
- **gain sd:** how much the gain moves within a song.

Use the `off` row as the baseline.

The Sims 3's own songs are `.snr`/`.sns` files in the game's `Sound` folders. Decode them to `.wav` first with [vgmstream](https://github.com/vgmstream/vgmstream), e.g. `vgmstream-cli -o song.wav song.snr`.

### Harness input format

`harness` reads a little-endian stream on stdin: chunks of a 4-byte tag, two `int32`s (an ID and a frame count), then the frames.

| Chunk | Contents |
|---|---|
| `SONG` | interleaved stereo `float32` frames at 44.1 kHz |
| `GAP ` | no data: that many frames of silence |
| `END ` | end of the stream |

It prints one line per song:

```
id  I_in  I_out  I15_in  I15_out  LRA_in  LRA_out  peak_in  peak_out  gain_sd  gain_range
```

The columns are integrated loudness (whole song and first 15 s, in LUFS), loudness range (in LU), sample peaks (in dBFS), and the standard deviation and range of the applied gain (in dB).

Any `LvParams` field can be overridden with `--<field> <value>`, e.g. `--targetLufs -14`. In addition:

- `--player 1` signals song changes exactly, like the in-game player tracking.
- `--bypass 1` measures with leveling off.
