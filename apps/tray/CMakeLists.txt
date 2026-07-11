# clipster-tray: the background application (WIN32 subsystem, no console).

add_executable(clipster-tray WIN32 main.cpp)

target_link_libraries(clipster-tray PRIVATE
  clipster_app_common
  user32
  shell32
  advapi32
)

if(MSVC)
  target_compile_options(clipster-tray PRIVATE /EHsc)
endif()

add_custom_command(TARGET clipster-tray POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:clipster-tray>/assets
)
