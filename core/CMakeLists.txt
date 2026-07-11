# clipster-core: platform-agnostic logic (ring buffer, settings, game
# matching). Depends only on nlohmann-json so it builds anywhere.

find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  include(FetchContent)
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(clipster_core STATIC
  src/logging.cpp
  src/ring_buffer.cpp
  src/settings.cpp
  src/game_matcher.cpp
  src/steam.cpp
  src/util.cpp
)

target_include_directories(clipster_core PUBLIC include)
target_link_libraries(clipster_core PUBLIC nlohmann_json::nlohmann_json)
