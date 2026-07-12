# Clipster.exe — the Qt application (main window + tray, hosts the
# recording engine).

find_package(Qt6 COMPONENTS Widgets CONFIG REQUIRED)

add_executable(clipster_gui WIN32
  main.cpp
  main_window.cpp
  settings_widget.cpp
)

set_target_properties(clipster_gui PROPERTIES OUTPUT_NAME "Clipster")

target_link_libraries(clipster_gui PRIVATE
  Qt6::Widgets
  clipster_app_common
  winmm
)

if(MSVC)
  target_compile_options(clipster_gui PRIVATE /EHsc)
endif()

add_custom_command(TARGET clipster_gui POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:clipster_gui>/assets
)
