add_executable(clipster main.cpp)

target_link_libraries(clipster PRIVATE clipster_app_common)

if(MSVC)
  target_compile_options(clipster PRIVATE /EHsc)
endif()

# Bundle sounds (and later icons) next to the executable.
add_custom_command(TARGET clipster POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:clipster>/assets
)
