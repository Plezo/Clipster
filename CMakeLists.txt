cmake_minimum_required(VERSION 3.24)

project(clipster
  VERSION 0.2.0
  DESCRIPTION "Lightweight background game replay buffer"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The media library (FFmpeg encode/mux) and the apps need third-party
# dependencies; the core library is dependency-light so it can be built and
# unit-tested on any platform without vcpkg.
option(CLIPSTER_BUILD_MEDIA "Build the FFmpeg-based media library" ON)
option(CLIPSTER_BUILD_APPS  "Build the applications (Windows only for now)" ${WIN32})
option(CLIPSTER_BUILD_TESTS "Build unit tests" ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
  add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_subdirectory(core)

if(CLIPSTER_BUILD_MEDIA)
  add_subdirectory(media)
endif()

if(WIN32 AND CLIPSTER_BUILD_APPS)
  add_subdirectory(platform/win)
  add_subdirectory(apps/common)
  add_subdirectory(apps/cli)
  add_subdirectory(apps/gui)
endif()

if(CLIPSTER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
