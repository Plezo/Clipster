# Shared frontend logic: the recording engine used by both the CLI and the
# tray app.

add_library(clipster_app_common STATIC
  recorder.cpp
)

target_include_directories(clipster_app_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(clipster_app_common
  PUBLIC clipster_core clipster_media clipster_platform_win
  PRIVATE winmm
)

if(MSVC)
  target_compile_options(clipster_app_common PRIVATE /EHsc)
endif()
