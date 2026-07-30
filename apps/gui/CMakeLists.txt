# Clipster.exe — the Qt application (main window + tray, hosts the
# recording engine).

find_package(Qt6 COMPONENTS Widgets Network Multimedia MultimediaWidgets CONFIG REQUIRED)

add_executable(clipster_gui WIN32
  main.cpp
  main_window.cpp
  settings_widget.cpp
  clips_page.cpp
  updater.cpp
  clipster.rc
)

set_target_properties(clipster_gui PROPERTIES OUTPUT_NAME "Clipster")

target_link_libraries(clipster_gui PRIVATE
  Qt6::Widgets
  Qt6::Network
  Qt6::Multimedia
  Qt6::MultimediaWidgets
  clipster_app_common
  winmm
)

target_compile_definitions(clipster_gui PRIVATE
  CLIPSTER_VERSION="${PROJECT_VERSION}"
)

if(MSVC)
  target_compile_options(clipster_gui PRIVATE /EHsc)
endif()

add_custom_command(TARGET clipster_gui POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:clipster_gui>/assets
)

# vcpkg's applocal step copies the Qt DLLs but not the Qt plugins; without
# platforms/qwindows.dll next to the exe, Qt cannot start ("no Qt platform
# plugin could be initialized"). Deploy the windeployqt-style layout.
set(_qt_plugin_src
  "$<IF:$<CONFIG:Debug>,${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/Qt6/plugins,${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins>")
# tls carries the TLS backends QNetworkAccessManager needs for https;
# multimedia carries the ffmpeg backend the Clips-tab player needs.
foreach(_plugin_dir platforms styles imageformats tls multimedia)
  add_custom_command(TARGET clipster_gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${_qt_plugin_src}/${_plugin_dir}"
      "$<TARGET_FILE_DIR:clipster_gui>/${_plugin_dir}"
  )
endforeach()

# Qt prefers the OpenSSL TLS backend when its plugin is present, and that
# plugin is loaded at runtime — so vcpkg's applocal step never sees its
# dependency on libssl and copies only libcrypto (which Qt6Network links
# directly). The half-deployed backend then fails to load without Qt
# falling back to Schannel, and every https request dies with "TLS
# initialization failed" — which is why the in-app update check has been
# silently skipped in every packaged build. Globbed, so an OpenSSL major
# bump does not break the build.
file(GLOB _openssl_runtime RELATIVE
  "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
  "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/libssl-*.dll")
set(_openssl_bin
  "$<IF:$<CONFIG:Debug>,${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin,${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin>")
foreach(_dll ${_openssl_runtime})
  add_custom_command(TARGET clipster_gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${_openssl_bin}/${_dll}"
      "$<TARGET_FILE_DIR:clipster_gui>"
  )
endforeach()
