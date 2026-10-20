# Changelog

## 1.3.0

First public release.

- **New defaults.** The shipped settings are now the ones tuned in-game:
  - `Strength=standard` (the former `max`)
  - `TargetLUFS=-16` (was -14)
  - `MaxBoostDB=10` (was 6)
  - `LogIntervalSec=30` (was 60)

  An existing `RadioLeveler.ini` keeps its own values. Delete it to get the new defaults.
- **Strength presets renamed** to `standard`, `relaxed`, `light` and `subtle`. The old names still work: `max`, `strong`, `balanced` and `gentle`.
- **`Logging=0`** stops the mod writing `RadioLeveler.log` at all. The log also starts over if it passes 256 KB.
- **Releases include `RadioLeveler.pdb`** (debug symbols) and the default `RadioLeveler.ini`, and are built by GitHub Actions from the tagged source.
- MIT license, Visual Studio project, documentation.

## 1.2.2

- **New `Strength=max` (now `standard`).** Pulls songs tighter and also evens out loud and quiet sections within a song.

## 1.2.1

- **No more false song splits.** When the game's sound player is tracked, the silence-based song-change fallback is off from the start, so a quiet passage can no longer split a song.
- **The A/B log line shows the gain being switched,** so you know how big a difference to listen for.

## 1.2.0

- **A/B hotkey (`ToggleHotkey`, default Ctrl+Shift+L).** Switches leveling off and on mid-song with a short crossfade. It only reacts while the game window is focused.
- **Clearer log.** Each song gets an "on" and an "off" line with its measured and played loudness. Songs skipped within about 8 seconds are logged as too short to measure.

## 1.1.0

- **Exact song changes.** Each radio's sound player (`SndPlayer1`) is watched, so skips are caught as well as natural song ends. Silence detection remains as the fallback.
- **Smoother starts.** A radio that turns on or changes station starts from the last song's level heard in the past 10 minutes, instead of from 0 dB.
- **Per-song log lines.** The periodic status line now defaults to every 60 s.

## 1.0.0

- First version. A per-song loudness leveler (ITU-R BS.1770) with a lookahead peak limiter, running in the radio's unused Compressor1 slot. Presets `gentle`, `balanced` (default) and `strong`.
