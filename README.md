# Clipster

Open-source, lightweight game clipping for Windows. Clipster sits in your
system tray, starts recording automatically when you launch a game, and
saves the last N seconds to an MP4 when you press the clip hotkey
(default **Ctrl+Del**) — like ShadowPlay, but minimal and yours.

Nothing is written to disk until you clip: video is hardware-encoded
(NVENC/AMF/QuickSync) into a rolling in-memory buffer, and a clip is a
millisecond-fast remux of that buffer — no re-encoding, no quality loss.

## Build

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

Run **`clipster-tray.exe`** (from `build\windows\apps\tray\RelWithDebInfo\`,
or copy that whole folder somewhere permanent, e.g.
`%LOCALAPPDATA%\Programs\Clipster`). An icon appears in the system tray:

1. **Launch a game.** Steam libraries are detected automatically; Clipster
   starts recording and shows a notification.
2. **Something cool happens → press `Ctrl+Del`.** The last 30 seconds
   (configurable) are saved to `Videos\Clipster\<Game>\` with a chime.
3. **Quit the game.** Recording stops and the buffer is freed.

Right-click the tray icon for: Save clip · Settings · Open clips folder ·
Start with Windows · Quit. Double-click opens the clips folder.

Configuration lives in a friendly UI (`clipster-settings.exe`, also on the
tray menu) or directly in `%APPDATA%\Clipster\settings.json` — hotkey,
clip length, buffer length, fps/bitrate/codec, save location, filename
template, watched game folders, extra/ignored executables, sounds. The
tray picks up changes automatically; recording settings apply to the next
game session.

Non-Steam games: add their install folders to *watched folders* or their
`.exe` to *always record* in settings.

Troubleshooting: the tray logs to `%APPDATA%\Clipster\clipster.log`. A
CLI harness also exists for testing capture on any window:
`clipster.exe --window "<title substring>"`.

## How it works / contributing

Capture is Windows.Graphics.Capture (injection-free, anticheat-friendly),
encoding is your GPU's hardware encoder via FFmpeg's libavcodec, and the
sliding window is a GOP-aligned ring buffer of already-encoded packets —
RAM cost ≈ `buffer_seconds × bitrate / 8` (120 s @ 20 Mbps ≈ 300 MB).
Layout: `core/` (platform-agnostic logic, unit-tested), `media/` (FFmpeg),
`platform/win/` (WGC/WASAPI/hotkeys), `apps/` (tray, settings UI, CLI).
See `CLAUDE.md` for developer docs. Core tests run anywhere:
`cmake --preset linux-core && cmake --build --preset linux-core && ctest --preset linux-core`.

## License

MIT. FFmpeg is dynamically linked with LGPL components only; the software
H.264 fallback is OpenH264 (BSD).
