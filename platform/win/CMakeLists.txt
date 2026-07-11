# clipster-platform-win: Windows implementations of capture (WGC), audio
# (WASAPI incl. per-process loopback), global hotkeys and shell helpers.

add_library(clipster_platform_win STATIC
  src/str_util.cpp
  src/known_folders.cpp
  src/window_finder.cpp
  src/wgc_capture.cpp
  src/hotkey_manager.cpp
  src/audio_capture.cpp
  src/process_watcher.cpp
  src/process_utils.cpp
  src/steam_locator.cpp
)

target_include_directories(clipster_platform_win PUBLIC include)
target_link_libraries(clipster_platform_win
  PUBLIC clipster_core
  PRIVATE d3d11 dxgi windowsapp user32 shell32 ole32 mmdevapi advapi32
)

# C++/WinRT generates coroutine-heavy headers; MSVC needs this for them.
if(MSVC)
  target_compile_options(clipster_platform_win PRIVATE /bigobj /EHsc)
endif()
