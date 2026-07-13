# Clipster

Open-source, lightweight game clipping for Windows. Clipster sits in your
system tray, starts recording automatically when you launch a game, and
saves the last N seconds to an MP4 when you press the clip hotkey
(default **Ctrl+Del**) — like ShadowPlay, but minimal and yours.

Nothing is written to disk until you clip: video is hardware-encoded
(NVENC/AMF/QuickSync) into a rolling in-memory buffer, and a clip is a
millisecond-fast remux of that buffer — no re-encoding, no quality loss.

## Download

From the [Releases page](../../releases):

- **`Clipster-Setup-*.exe`** — one-click installer (per-user, no admin
  needed; adds a Start menu entry and uninstaller). Recommended.
- **`Clipster-*-windows-x64.zip`** — portable: unzip anywhere, run
  `Clipster.exe`.

Updating: run the new installer over the old install — it closes a
running Clipster, replaces it in place, and offers to relaunch. Clipster
checks GitHub once at startup and shows an "Update available" link on
the Home page when there is a newer release.

## Build from source

Prerequisites (one-time):

- Visual Studio 2022 with the **Desktop development with C++** workload
- CMake 3.24+
- [vcpkg](https://github.com/microsoft/vcpkg) cloned and bootstrapped, with
  the `VCPKG_ROOT` environment variable pointing at it

```powershell
git clone https://github.com/<you>/clipster
cd clipster
cmake --preset windows            # first run compiles FFmpeg + Qt via vcpkg (slow once)
cmake --build --preset windows-release
```

Everything lands in `build\windows\apps\<app>\RelWithDebInfo\`.

## Use

Run **`Clipster.exe`** (from `build\windows\apps\gui\RelWithDebInfo\`, or
copy that whole folder somewhere permanent, e.g.
`%LOCALAPPDATA%\Programs\Clipster`). The main window shows live recording
status and your recent clips; the Settings page has everything else:

1. **Launch a game.** Steam libraries are detected automatically; the
   status flips to "● Recording" with live buffer stats.
2. **Something cool happens → press `Ctrl+Del`** (or `Back+RB` on a
   controller, or the Save clip button). The last 30 seconds
   (configurable) are saved to `Videos\Clipster\<Game>\` with a chime,
   and appear in the recent-clips list — double-click to play.
3. **Quit the game.** Recording stops and the buffer is freed.

Closing the window minimizes Clipster to the system tray so it keeps
recording in the background; tick *Start with Windows* in Settings and
you never have to think about it again.

Settings cover the hotkey and controller combo, clip length (which is
also how much gameplay is kept in memory), fps/bitrate/codec, save
location, filename template, audio mode, microphone, watched game
folders, extra/ignored executables, and sounds. (They are
stored in `%APPDATA%\Clipster\settings.json` if you prefer a text
editor.) Recording settings apply to the next game session.

Non-Steam games: add their install folders to *watched folders* or their
`.exe` to *always record* in settings.

**Audio** is captured per-application (Windows 10 2004+): record everything,
the game only, the game plus chosen apps (default: Discord), or everything
except chosen apps (e.g. Spotify) — pick the mode in Settings → Audio.

Troubleshooting: Clipster logs to `%APPDATA%\Clipster\clipster.log`. If
the hotkey works on the desktop but not in a specific game, that game is
probably running as administrator — run Clipster as administrator too
(Windows withholds input from elevated windows otherwise). The controller
combo is unaffected by this. A CLI harness also exists for testing
capture on any window: `clipster-cli.exe --window "<title substring>"`.

## How it works / contributing

Capture is Windows.Graphics.Capture (injection-free, anticheat-friendly),
encoding is your GPU's hardware encoder via FFmpeg's libavcodec, and the
sliding window is a GOP-aligned ring buffer of already-encoded packets —
RAM cost ≈ `clip_seconds × bitrate / 8` (30 s @ 20 Mbps ≈ 75 MB).
Layout: `core/` (platform-agnostic logic, unit-tested), `media/` (FFmpeg),
`platform/win/` (WGC/WASAPI/hotkeys), `apps/` (tray, settings UI, CLI).
See `CLAUDE.md` for developer docs. Core tests run anywhere:
`cmake --preset linux-core && cmake --build --preset linux-core && ctest --preset linux-core`.

## License

MIT. FFmpeg is dynamically linked with LGPL components only; the software
H.264 fallback is OpenH264 (BSD).
