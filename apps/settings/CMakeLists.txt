# clipster-settings: Qt Widgets settings editor. Reads/writes the same
# settings.json as the tray app, which hot-reloads it on save.

find_package(Qt6 COMPONENTS Widgets CONFIG REQUIRED)

add_executable(clipster-settings WIN32 main.cpp)

target_link_libraries(clipster-settings PRIVATE
  Qt6::Widgets
  clipster_core
  winmm
)

if(MSVC)
  target_compile_options(clipster-settings PRIVATE /EHsc)
endif()
