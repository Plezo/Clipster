# clipster-media: FFmpeg-based hardware encoding and MP4 muxing.
# FFmpeg is used as a thin wrapper over the native encoders (NVENC/AMF/QSV)
# plus container writing — no software scaling/encoding in the hot path
# unless no hardware encoder exists.

find_package(FFMPEG QUIET)  # provided by vcpkg's ffmpeg port
if(NOT FFMPEG_FOUND)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(AV REQUIRED IMPORTED_TARGET
    libavcodec libavformat libavutil libswscale libswresample)
endif()

add_library(clipster_media STATIC
  src/video_encoder.cpp
  src/audio_encoder.cpp
  src/clip_writer.cpp
)

target_include_directories(clipster_media PUBLIC include)
target_link_libraries(clipster_media PUBLIC clipster_core)

if(FFMPEG_FOUND)
  target_include_directories(clipster_media PRIVATE ${FFMPEG_INCLUDE_DIRS})
  target_link_directories(clipster_media PUBLIC ${FFMPEG_LIBRARY_DIRS})
  target_link_libraries(clipster_media PUBLIC ${FFMPEG_LIBRARIES})
else()
  target_link_libraries(clipster_media PUBLIC PkgConfig::AV)
endif()
