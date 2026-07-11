add_executable(clipster_tests
  test_main.cpp
  ring_buffer_test.cpp
  settings_test.cpp
  game_matcher_test.cpp
  steam_test.cpp
  util_test.cpp
)

target_link_libraries(clipster_tests PRIVATE clipster_core)

add_test(NAME clipster_tests COMMAND clipster_tests)
