# How RadioLeveler works

> Written by Claude, Anthropic's AI model, from its own analysis of the game. See "How this was made" in the [README](../README.md#how-this-was-made).

This is the developer's view: where the leveler sits in the game's audio engine, the plug-in interface it relies on, the leveling algorithm, and how it was measured. Player-facing documentation is in the [README](../README.md).

## Overview

The Sims 3 plays sound through RenderWare Audio Core (RWAC). Every sound is a *voice* built from a *voice template*: a list of DSP plug-ins, each identified by a four-character code, through which the voice's audio is processed in 256-frame blocks.

RadioLeveler has two parts:

1. **`RadioLeveler.package`.** Overrides the radio's voice template to add Compressor1 (`Co10`) to the chain. Co10 is registered by the game but unused in vanilla. Its default threshold is 120 dB, so on its own it passes audio through unchanged.
2. **`RadioLeveler.asi`.** Finds Co10's plug-in descriptor in memory and swaps three of its function pointers (GetSize, Init and Process). From then on, every Co10 instance runs the leveler in [`src/leveler.h`](../src/leveler.h) instead.

No game code is patched, and only voices built from the overridden template are affected. With only one of the two files installed, the game sounds exactly as vanilla.

## The voice template

The package holds a single resource: VOCE (type `0x029E333B`), group `0x001407EC`, instance `0x3C18C0DE65EC32C9`. This is the game's `stereo` voice template, which only radio music uses. It is a short text resource:

```
1
SnP1 Rch0 Rsp0 Pau0 TSt0 Co10 LI20 HI20 Sen0[reverb] Sen0
lowpass=LI20:0,96000
hipass=HI20:0,0
wetlevel=Sen0[reverb]:0,0
mixpitch=Rsp0:0
```

The second line is the plug-in chain. The package inserts `Co10` after the time-stretch stage (`TSt0`) and before the filters and sends. The remaining lines bind the template's named properties to plug-in attributes; property names are looked up by their FNV-32 hash, in lowercase. They are copied unchanged from the vanilla template.

The final `Sen0` applies the emitter's volume, which carries the stereo's Low/Medium/High setting, the Music slider and distance falloff. All of them act after the leveler, so they still work as before. `tools/make_package.py` rebuilds the package from this text.

## Plug-in interface

These offsets were recovered from the EA App build, using the game's own scripts, its sound resources and a memory dump of the running game. The ASI depends on the following:

| Object | Offset | Meaning |
|---|---|---|
| descriptor | `+0x00` | name (`"Compressor1"`) |
| | `+0x04` | `unsigned GetSize(spec*)`: instance size in bytes |
| | `+0x08` | `bool Init(inst, param)` |
| | `+0x10` | `int Process(inst, ctx)`; cdecl, returns 1 |
| | `+0x24` | next descriptor in the registry |
| | `+0x28` | four-cc ID as a little-endian dword (`'Co10'` = `0x436F3130`) |
| instance | `+0x08` | owning voice |
| | `+0x10` | descriptor |
| | `+0x21` | `u8` channel count |
| voice | `+0x44` | `u8` plug-in count |
| | `+0x4C` | plug-in instance pointers, in chain order |
| process ctx | `+0x3000C` | current buffer\* |
| | `+0x30010` | scratch buffer\* |
| | `+0x30018` | → object with `float` sample rate at `+0x0C` |
| buffer | `+0x04` | `float*` planar data |
| | `+0x0E` | `u16` channel stride in floats |
| SndPlayer1 | `+0x1AC` | handle of the sample now playing |
| | `+0x1C9` | `u8` index of the current request in its queue |

A plug-in reads the current buffer, writes the scratch buffer, and swaps the two pointers.

### Hooking

The steps in order:

1. **Find the descriptor.** `InstallHooks()` scans the executable's writable sections for a dword equal to the four-cc. It accepts a match only if the name pointer at `-0x28` points at `"Compressor1"` and GetSize, Init and Process all point into executable sections.
   - The EA App `TS3.exe` is encrypted on disk and decrypts itself at startup, so the scan retries every 100 ms for up to 3 minutes.
2. **Check the instance size.** It calls the stock GetSize. The vanilla value is `0xC0`; anything outside `0x28..0x4000` aborts the hook.
3. **Swap the pointers.** It swaps `desc[1]`, `desc[2]` and `desc[4]` with `InterlockedExchange`.

What each replacement does:

- **`RL_GetSize`** returns the stock size, rounded up to 16, plus `sizeof(InstExt)` (about 12 KB). The leveler's state lives after the stock instance data.
- **`RL_Init`** runs the stock Init and then tags the extension with a magic value.
- **`RL_Process`** falls back to the stock Process in any of these cases: disabled, magic missing, a null buffer, more than 8 channels, or a stride under 256. At its default threshold the stock Process is a no-op.

The ASI only activates inside `TS3.exe` or `TS3W.exe`.

### Song changes

In practice the game creates a new voice for each song. The leveler still watches the voice's `SndPlayer1`, the first plug-in in the chain, so a voice that moves on to another song is caught too:

- **Finding the player.** `FindPlayer()` checks that the instance is in its voice's plug-in list and that plug-in 0's descriptor is SndPlayer1. The pointers are validated with `VirtualQuery` first, and any mismatch disables tracking for that voice.
- **Detecting a change.** A change in the playing sample handle or the queue index starts a new song measurement (`lv_song_changed`).
- **Fallback.** When the player isn't found, song changes are guessed from silence (at least 250 ms below -45 LUFS) or a sustained level jump (6 dB for 4 s). When the player is found, both guesses are turned off, because a quiet passage could otherwise split a song.

A new voice is seeded with the last settled song loudness heard on any radio in the past 10 minutes, so it starts near the right gain instead of at 0 dB.

### Steam and EA App

The Steam `TS3W.exe` is not encrypted. Its RWAC functions are identical to the EA App build's apart from their addresses, so the same scan finds the descriptors in both:

| | Co10 descriptor | SnP1 descriptor |
|---|---|---|
| EA App `TS3.exe` | `0x011F938C` | `0x011FD3B4` |
| Steam `TS3W.exe` | `0x011A350C` | `0x011A7534` |

## The leveler

[`src/leveler.h`](../src/leveler.h) is header-only C and needs no allocation or locks. It processes 256-frame planar blocks, with up to 8 channels at any sample rate from 8 to 384 kHz. Radio voices are 2-channel 48 kHz.

1. **Loudness measurement.** The ITU-R BS.1770 K-weighting filter (re-derived for the actual sample rate) feeds three exponential detectors of mean-square power:
   - fast, 50 ms: detects gaps;
   - short, 0.4 s: gating;
   - medium, 3 s: detects level jumps.
2. **Gating.** A block counts toward the song estimate only if its short-term loudness is above -50 LUFS and no more than 20 dB below the current estimate. This keeps fades, quiet intros and gaps from dragging it down.
3. **Song estimate.** Gated block powers are accumulated with a forgetting factor (`forgetSec`, 8 s in `standard`), giving a running integrated loudness for the song.
4. **Blend with the previous song.** At a song change, the old estimate becomes a prior worth `priorUpSec` seconds (new song quieter) or `priorDownSec` seconds (new song louder) of measurement. The asymmetry lets loud songs come down fast while quiet intros aren't boosted straight away.
5. **Gain.** The target is `targetLufs - estimate`, clamped to `[-maxCutDb, +maxBoostDb]`. The applied gain moves toward it at no more than `upDbps` up or `downDbps` down, interpolated per sample.
6. **Limiter.** A 3 ms lookahead peak limiter catches peaks above the ceiling (-1 dBFS) after gain. Each sample's required gain feeds a sliding minimum over the lookahead window. That minimum is box-averaged over the same window, which gives a smooth attack that is fully down by the time the peak arrives, and then releases with a 120 ms time constant.
7. **A/B.** The hotkey sets a wet target of 0 or 1, and the wet amount moves a third of the way per block, about 16 ms. At wet 0 the output is the input delayed by the lookahead, bit for bit. The detectors and limiter keep running, so switching back lands on the right gain at once.

### Presets

`Strength` sets five of the parameters. `lv_default_params()` is `standard`.

| | `standard` | `relaxed` | `light` | `subtle` |
|---|---|---|---|---|
| `priorUpSec` | 1.5 | 3 | 10 | 15 |
| `priorDownSec` | 0.5 | 1 | 2 | 2 |
| `upDbps` | 6 | 4 | 1.5 | 1 |
| `downDbps` | 24 | 20 | 6 | 6 |
| `forgetSec` | 8 | 20 | 60 | 120 |
| old name (still accepted) | `max` | `strong` | `balanced` | `gentle` |

All of them can be overridden one by one in the ini; see the commented "Advanced" block.

### Threads

- **Audio:** the game's mixer thread calls `RL_Process`. Parameters are double-buffered with a generation counter, so an ini reload never blocks the audio.
- **Monitor (1 s):** reloads the ini when its timestamp changes and writes the per-song log lines from telemetry slots the audio thread fills.
- **Hotkey (25 ms):** polls `GetAsyncKeyState`, and only while the game window has focus.

## Measurements

The offline harness runs a stream of songs through `leveler.h` exactly as the game would, in 256-frame blocks at 44.1 kHz. It measures each song's BS.1770 integrated loudness and LRA before and after. Its meter was checked against ffmpeg's `ebur128` filter.

The test set was 69 songs, drawn at random from the EA stations (decoded with vgmstream) and a custom-music folder. Song changes were signalled the way the player tracking does it.

| | Spread p5–p95 | Loudest − quietest | Median LRA | In-song gain movement (sd) |
|---|---|---|---|---|
| no leveling | 4.5 dB | 9.7 dB | 4.9 LU | – |
| `standard` | 0.6 dB | 1.3 dB | 3.4 LU | 1.2 dB |
| `relaxed` | 0.9 dB | 1.6 dB | 4.1 LU | 1.0 dB |
| `light` | 1.6 dB | 2.2 dB | 4.5 LU | 0.7 dB |
| `subtle` | 1.8 dB | 2.5 dB | 4.6 LU | 0.6 dB |

- **Skip test:** with every song cut after a random 20–60 s and no gap, the spread with `standard` goes from 7.9 dB to 1.2 dB (3.0 dB with `light`).
- **Peaks:** output peaks stay at or below -1 dBFS.
- **Cost:** one stereo voice costs about 0.1 % of one core.

The test programs are described in [tests/README.md](../tests/README.md).
