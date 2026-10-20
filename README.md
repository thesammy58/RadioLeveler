# RadioLeveler

ASI mod for The Sims 3 that levels every radio song to the same loudness, on EA's stations and Custom Music alike, so you stop reaching for the stereo's volume every few songs. Requires an ASI loader.

> **Made with AI.** The research, reverse engineering, code and documentation in this repository were produced by Claude, Anthropic's AI model, in conversations with me. I came up with the idea, supplied the game files and test data, tested every build in game and tuned the defaults. Details are under [How this was made](#how-this-was-made).

## The problem

Songs on the Sims 3 radio are nowhere near the same loudness. EA's own stations span about 10 dB from the quietest song to the loudest, and custom music is often louder still. Normalizing your MP3s to the same average level doesn't fix it either, because average level isn't the same as how loud a song sounds, and the game's radio has no loudness normalization of its own.

RadioLeveler measures each song while it plays and steers it to one target loudness, much like the volume normalization on streaming services:

- **Covers every music player:** stereos, wired speakers, jukeboxes and boomboxes.
- **Song by song:** skips included, with a log line for each song.
- **Leaves the rest of the game alone:** no other game sound is touched. Your stereo's Low/Medium/High setting, distance falloff and the Music slider still work as usual, because they're applied after the leveler.

## Install

1. Make sure you have an ASI loader in `The Sims 3\Game\Bin`, for example [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/latest) or the one that comes with [Smooth Patch](https://modthesims.info/d/658759/smooth-patch-2-1.html).
2. Download the latest zip from [Releases](../../releases). It has two folders:
   - `Game\Bin`: copy its files into `The Sims 3\Game\Bin`.
   - `Mods\Packages`: copy `RadioLeveler.package` into `Documents\Electronic Arts\The Sims 3\Mods\Packages`.
3. Launch the game, turn on a stereo, and open `RadioLeveler.log` in `Game\Bin`. A line per song, like `radio #1 on (2ch 48000 Hz)`, means it's working.

What's in `Game\Bin`:
- **`RadioLeveler.asi`** is the mod.
- **`RadioLeveler.ini`** holds the settings. If you're upgrading and have changed yours, keep your copy.
- **`RadioLeveler.pdb`** holds debug symbols and is optional. Keep it next to the `.asi` if you use a debugger or crash logger, so it can show this mod's function names.

Both the `.asi` and the `.package` are needed. With only one installed, the game plays exactly as vanilla.

To uninstall, delete `RadioLeveler.asi`, `.ini`, `.pdb` and `.log` from `Game\Bin`, and `RadioLeveler.package` from `Mods\Packages`.

The release downloads are built automatically by GitHub from the source code in this repository, so what you download matches the code you can read here.

## Compatibility

| Game version | Status |
| --- | --- |
| EA App / Origin 1.69.47 (`TS3.exe`) | Tested in game |
| Steam 1.67.2 (`TS3W.exe`) | Should work, not yet tested in game. The audio code was checked against the Steam executable and matches. |
| Disc 1.67.2 (`TS3W.exe`) | Untested. The mod finds the audio plug-in by scanning memory, not by fixed address. |

It has been tested alongside Sims 3 Settings Setter, Sims3MonoModder and several other ASI mods. It doesn't patch any game code or scripts. The package overrides one resource, the radio's `stereo` voice template, so the only possible conflict is another mod that replaces that same resource.

## Using it

- **Ctrl+Shift+L** switches leveling off and on while the game window is focused, so you can compare. Off means the game's original levels, with a short crossfade. The log says how big a difference to listen for, e.g. `A/B: leveling OFF - original game levels (difference you should hear: radio #3 +6.0 dB)`.
- **Changes to `RadioLeveler.ini`** are picked up while the game runs, within about 2 seconds. If the file is missing, the mod writes a default one.

| Setting | Default | What it does |
| --- | --- | --- |
| `Enabled` | `1` | `0` passes radio audio through untouched |
| `Strength` | `standard` | How hard songs are pulled together; see below |
| `TargetLUFS` | `-16` | The loudness every song is steered to, before the stereo's own volume. Higher is louder overall (e.g. `-14`), lower is quieter (e.g. `-18`). |
| `MaxBoostDB` | `10` | The most a quiet song is turned up |
| `MaxCutDB` | `12` | The most a loud song is turned down |
| `LimiterCeilingDB` | `-1` | Peak ceiling for boosted songs; `0` turns the limiter off |
| `ToggleHotkey` | `Ctrl+Shift+L` | Ctrl/Shift/Alt plus a letter, digit, F1–F24, Insert, Home, End, PageUp, PageDown, Delete, Pause or ScrollLock. `None` disables it. |
| `Logging` | `1` | `0` stops the mod writing `RadioLeveler.log` at all |
| `LogSongs` | `1` | A line when each song starts and ends |
| `LogIntervalSec` | `30` | An extra status line per playing radio every N seconds; `0` turns it off |

The log starts over at each game launch, and again if it passes 256 KB.

### Strength

| | No leveling | `standard` (default) | `relaxed` | `light` | `subtle` |
| --- | --- | --- | --- | --- | --- |
| Song-to-song loudness spread (5th–95th percentile) | 4.5 dB | 0.6 dB | 0.9 dB | 1.6 dB | 1.8 dB |
| Loudest vs. quietest song | 9.7 dB | 1.3 dB | 1.6 dB | 2.2 dB | 2.5 dB |
| Loud/quiet contrast within a song (LRA, median) | 4.9 LU | 3.4 LU | 4.1 LU | 4.5 LU | 4.6 LU |

- **`standard`** pulls songs tightest and also evens out loud and quiet sections within a song, a bit like FM radio processing.
- **The lighter presets** keep more of each song's own dynamics and mainly fix the jumps between songs.
- **Old names still work:** `max` = standard, `strong` = relaxed, `balanced` = light, `gentle` = subtle.

The numbers come from 69 songs drawn at random from the EA stations plus custom MP3s, run through the exact code in the ASI. With skipping (each song cut after 20–60 s), `standard` brings the spread from 7.9 dB down to 1.2 dB.

### Reading the log

```
radio #12 on (2ch 48000 Hz) | starting at +2.5 dB, carried over from the last song
radio #12 off | song measured -17.6 LUFS -> played at -16.0 LUFS (gain +1.6 dB)
```

- **"measured"** is how loud the song is in the game without the mod. **"played at"** is what you heard.
- **The `#` counts sound voices, not stereos.** The game creates a new voice for every song, so one stereo shows up as #1, #2, #3… Each song starts from the previous song's level.
- **A song skipped within a few seconds** is logged as `too short to measure`.

### FAQ

**I can't hear any difference with the hotkey.**
Check whether Windows **Loudness Equalization** is on (Control Panel → Sound → Playback → your device → Properties → Enhancements). It levels everything the PC plays, so it hides the difference. Also compare on a song whose log line shows a gain of several dB; corrections under about 2 dB are hard to hear in a quick switch.

**The radio is louder or quieter overall than I'm used to.**
Change `TargetLUFS`. One step is about 1 dB.

**What does it cost?**
About 0.1 % of one CPU core per playing radio, on the game's audio thread, and about 12 KB of memory.

## How it works

- **Every sound is a voice.** The game's sound engine (RenderWare Audio Core) plays each sound through a chain of DSP plug-ins listed in a voice template. Only radio music uses the `stereo` template.
- **The package adds a plug-in to that chain.** It adds the engine's built-in compressor, which the vanilla game registers but never uses. At its default settings it does nothing.
- **The ASI takes over that plug-in.** It finds the compressor's plug-in entry in memory and points it at its own leveler, a BS.1770 loudness meter with a lookahead peak limiter. No game code is changed.
- **Song changes are tracked exactly.** The leveler watches the voice's sound player, so it knows when a song changes, whether it ended or was skipped. Each song is then measured as it plays and steered to the target, starting from the previous song's level.

[docs/TECHNICAL.md](docs/TECHNICAL.md) has the full write-up: the plug-in interface, the algorithm and how it was measured.

## Building

- **Clang with MinGW-w64 (what releases use):** run `./build.sh` to get `dist/RadioLeveler.asi`, `dist/RadioLeveler.pdb` and `dist/RadioLeveler.ini`. Then `python3 tools/make_release.py` packs the release zip.
  - Releases use [llvm-mingw 20260922](https://github.com/mstorsjo/llvm-mingw/releases/tag/20260922) (msvcrt). With it the build is reproducible: you get byte-identical files, so you can check a download against `SHA256SUMS.txt`.
  - Without llvm-mingw, `build.sh` uses the system Clang. On Debian or Ubuntu, install it with `sudo apt-get install clang lld g++-mingw-w64-i686-win32`.
- **Visual Studio:** open `RadioLeveler.slnx` and build Release | x86. The output is already named `.asi`. The Visual Studio build hasn't been tested in game yet.
- **The package:** `python3 tools/make_package.py` regenerates `package/RadioLeveler.package` from the voice template text.
- **The default ini:** `RadioLeveler.ini` must match the defaults built into the ASI. `python3 tools/sync_ini.py --write` regenerates it from the source.
- **Tests:** [tests/README.md](tests/README.md) covers them, including how to measure the leveler on your own music.

Version history is in [CHANGELOG.md](CHANGELOG.md).

## How this was made

This mod was designed and built in a series of conversations with Claude, Anthropic's AI model, in October 2026. Who did what:

**Claude (AI)**
- Worked out how the radio's audio is produced, from the game's decompiled scripts, its sound resources and a memory dump of the running game, and found the unused audio plug-in the mod takes over.
- Designed the leveling algorithm and measured it offline on real songs.
- Wrote all of the code in this repository (the ASI, the package generator, the tests, the build files) and the documentation, including this README.

**Me (thesammy58)**
- Came up with the idea and supplied the game files, the reference mods and the memory dump.
- Tested every build in game, decided how it should behave, and tuned the default settings by ear.

**What's verified:** in game on EA App 1.69.47, alongside other ASI mods: song-by-song leveling, skips, and switching it off and on with the hotkey. The Steam executable was checked to contain the same audio code.

**What isn't yet:** Steam and disc builds in game, and the Visual Studio build.

Commits written by Claude carry a `Co-Authored-By: Claude` line.

## Credits

- **sims3fiend and the [Sims 3 Settings Setter](https://github.com/sims3fiend/Sims3SettingsSetter) contributors**, and **Sims3MonoModder**, whose sources showed how ASI mods hook the game.
- **ts3.rebuild**, the decompiled game scripts used to trace how the radio plays music.
- **[vgmstream](https://github.com/vgmstream/vgmstream)**, which decoded EA's radio songs for the offline loudness tests.
- **ITU-R BS.1770 and EBU R128**, the loudness standards the leveler is built on.

## License

MIT. See [LICENSE](LICENSE).
