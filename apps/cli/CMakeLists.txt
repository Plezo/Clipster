# Dev harness; "clipster-cli" so it cannot collide with Clipster.exe when
# both are copied into one folder.
add_executable(clipster_cli main.cpp)
set_target_properties(clipster_cli PROPERTIES OUTPUT_NAME "clipster-cli")

target_link_libraries(clipster_cli PRIVATE clipster_app_common)

if(MSVC)
  target_compile_options(clipster_cli PRIVATE /EHsc)
endif()

# Bundle sounds (and later icons) next to the executable.
add_custom_command(TARGET clipster_cli POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:clipster_cli>/assets
)
