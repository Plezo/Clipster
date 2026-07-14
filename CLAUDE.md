# CLAUDE.md

Clipster is a Windows background game replay buffer (ShadowPlay-style):
continuous capture into an in-RAM ring buffer of encoded video, hotkey saves
the last N seconds as MP4 via remux (never re-encode). See README.md for the
user-facing story.

## Build & test

Windows (full build — requires VS2022, CMake, vcpkg with `VCPKG_ROOT` set):

```powershell
cmake --preset windows                      # configure + vcpkg deps
cmake --build --preset windows-release
ctest --preset windows
```

Executables land in `build\windows\apps\<name>\RelWithDebInfo\`.

Core-only build + unit tests work on any OS (no vcpkg):

```bash
cmake --preset linux-core && cmake --build --preset linux-core && ctest --preset linux-core
```

In a Linux sandbox without CMake, compile the core tests directly (needs the
nlohmann/json single header on the include path):

```bash
g++ -std=c++20 -Wall -Wextra -Wpedantic -Icore/include -I<json-include> -Itests \
    core/src/*.cpp tests/*.cpp -o /tmp/clipster_tests && /tmp/clipster_tests
```

## Architecture

- `core/` — platform-agnostic, dependency-light (nlohmann-json only), fully
  unit-tested: `SegmentRingBuffer` (GOP-aligned sliding window of
  `EncodedPacket`s), `Settings` (versioned JSON, tolerant load + clamp),
  `GameMatcher` (path rules), Steam `libraryfolders.vdf` parser.
- `media/` — FFmpeg wrapper. `VideoEncoder` probes NVENC → AMF → QSV →
  OpenH264 (never x264: LGPL cleanliness). `write_clip` remuxes ring-buffer
  packets to MP4 (temp file + rename; `+faststart`).
- `platform/win/` — WGC capture, WASAPI audio (desktop loopback + per-process
  loopback), global hotkeys, process watcher, Steam locator. All UTF-8 at the
  API surface (`win::widen`/`narrow` at Win32 boundaries).
- `apps/common/` — the engine shared by frontends: `Recorder` (video ring
  buffer + clip writing), `AudioPipeline`/`AudioMixer` (per-app WASAPI ->
  AAC), `SessionManager` (game detection -> session lifecycle on its own
  control thread; UI-agnostic callbacks).
- `apps/gui/` — `Clipster.exe`, the product: Qt main window (status,
  recent clips, settings pages) hosting the engine; minimizes to tray,
  `--minimized` for autostart, logs to `%APPDATA%\Clipster\clipster.log`.
- `apps/cli/` — `clipster-cli.exe`, dev/test harness.

Settings live at `%APPDATA%\Clipster\settings.json`, shared by all frontends.

## Conventions & gotchas

- C++20, MSVC `/W4` clean and GCC `-Wall -Wextra -Wpedantic` clean.
- Timestamps: everything is QPC-derived microseconds (WGC
  `SystemRelativeTime`, WASAPI QPC positions, `steady_clock`) so A/V share
  one clock. Video pts is rebased to the first captured frame.
- Ring buffer only accepts packets after the first video keyframe; clips
  always start on a GOP boundary.
- FFmpeg is 8.x via vcpkg: `AVCodec::pix_fmts` is deprecated — use
  `avcodec_get_supported_config` (see `video_encoder.cpp` for the pattern).
- Paths handed to FFmpeg must be UTF-8 (`path.u8string()`), never
  `path.string()` (ANSI on MSVC).
- WASAPI process loopback (`AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`)
  needs Win10 2004+ and a heap-allocated, properly refcounted completion
  handler (late callbacks after timeout — see `ActivationWaiter`).
- Development happens partly from WSL, which cannot compile `media/`,
  `platform/win/`, or `apps/` (no MSVC/FFmpeg there). Always run the core
  g++ test loop after touching `core/`; the user builds the rest on Windows
  and pastes errors.
- Hotkey strings ("Ctrl+Del") parse in `hotkey_manager.cpp`; keep the token
  set compatible with Qt `QKeySequence::toString` output.
- Releasing: bump `project(VERSION ...)` in the root CMakeLists.txt to match
  the tag before pushing `v*` — the release workflow rejects mismatches
  (the in-app update check compares that version against the latest tag).
  The tag builds a portable zip and an Inno Setup installer
  (`installer/clipster.iss`). Faster path: `scripts/release-local.ps1`
  builds/packages/publishes from the dev machine via `gh`; the release
  workflow's precheck job then skips the cloud rebuild.
