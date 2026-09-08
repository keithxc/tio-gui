cmake_minimum_required(VERSION 3.20)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" TIO_GUI_VERSION LIMIT_COUNT 1)

project(
  tio-gui
  VERSION "${TIO_GUI_VERSION}"
  DESCRIPTION "A lightweight, reliable GUI for tio"
  LANGUAGES C
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(Gettext REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.12)
pkg_check_modules(VTE REQUIRED IMPORTED_TARGET vte-2.91-gtk4>=0.74)
# VteRegex compile flags are PCRE2 compile flags; take them from the header
# rather than hard-coding the numeric values.
pkg_check_modules(PCRE2 REQUIRED IMPORTED_TARGET libpcre2-8)
pkg_check_modules(SOUP REQUIRED IMPORTED_TARGET libsoup-3.0>=3.0)
pkg_check_modules(JSON_GLIB REQUIRED IMPORTED_TARGET json-glib-1.0>=1.6)

add_executable(tio-gui
  src/highlighter.c
  src/payload.c
  src/quick_presets.c
  src/sequence.c
  src/serial_options.c
  src/connection_state.c
  src/log_model.c
  src/analyzer.c
  src/capture.c
  src/transfer.c
  src/plugin.c
  src/plugin_ui.c
  src/network.c
  src/network_ui.c
  src/modbus.c
  src/modbus_ui.c
  src/mqtt.c
  src/mqtt_ui.c
  src/main.c
  src/settings.c
)

include(CTest)
if(BUILD_TESTING)
  add_executable(mqtt-ui-test tests/mqtt_ui_test.c src/mqtt.c src/network.c src/payload.c src/log_model.c src/analyzer.c src/capture.c src/plugin.c src/plugin_ui.c)
  target_include_directories(mqtt-ui-test PRIVATE src)
  target_link_libraries(mqtt-ui-test PRIVATE PkgConfig::GTK4 PkgConfig::PCRE2 PkgConfig::JSON_GLIB)
  add_executable(mqtt-test tests/mqtt_test.c src/network.c)
  target_include_directories(mqtt-test PRIVATE src)
  target_link_libraries(mqtt-test PRIVATE PkgConfig::GTK4)
  add_executable(modbus-test tests/modbus_test.c src/modbus.c)
  target_include_directories(modbus-test PRIVATE src)
  target_link_libraries(modbus-test PRIVATE PkgConfig::GTK4)
  add_test(NAME modbus COMMAND modbus-test)
  add_executable(modbus-ui-test tests/modbus_ui_test.c src/modbus.c src/payload.c)
  target_include_directories(modbus-ui-test PRIVATE src)
  target_link_libraries(modbus-ui-test PRIVATE PkgConfig::GTK4)
  add_executable(network-ui-test tests/network_ui_test.c src/network.c src/payload.c src/log_model.c src/analyzer.c src/capture.c src/plugin.c src/plugin_ui.c)
  target_include_directories(network-ui-test PRIVATE src)
  target_link_libraries(network-ui-test PRIVATE PkgConfig::GTK4 PkgConfig::PCRE2 PkgConfig::JSON_GLIB)
  add_executable(network-test tests/network_test.c src/network.c)
  target_include_directories(network-test PRIVATE src)
  target_link_libraries(network-test PRIVATE PkgConfig::GTK4)
  add_executable(plugin-ui-test tests/plugin_ui_test.c src/plugin.c)
  target_include_directories(plugin-ui-test PRIVATE src)
  target_link_libraries(plugin-ui-test PRIVATE PkgConfig::GTK4)
  add_executable(plugin-test tests/plugin_test.c src/plugin.c)
  target_include_directories(plugin-test PRIVATE src)
  target_link_libraries(plugin-test PRIVATE PkgConfig::GTK4)
  add_executable(transfer-test tests/transfer_test.c src/transfer.c)
  target_include_directories(transfer-test PRIVATE src)
  target_link_libraries(transfer-test PRIVATE PkgConfig::GTK4)
  add_executable(capture-test tests/capture_test.c src/capture.c)
  target_include_directories(capture-test PRIVATE src)
  target_link_libraries(capture-test PRIVATE PkgConfig::GTK4 PkgConfig::JSON_GLIB)
  add_test(NAME capture COMMAND capture-test)
  add_executable(analyzer-test tests/analyzer_test.c src/log_model.c src/capture.c src/plugin.c src/plugin_ui.c)
  target_include_directories(analyzer-test PRIVATE src)
  target_link_libraries(analyzer-test PRIVATE PkgConfig::GTK4 PkgConfig::PCRE2 PkgConfig::JSON_GLIB)
  add_executable(log-model-test tests/log_model_test.c src/log_model.c)
  target_include_directories(log-model-test PRIVATE src)
  target_link_libraries(log-model-test PRIVATE PkgConfig::GTK4 PkgConfig::PCRE2 PkgConfig::JSON_GLIB)
  add_test(NAME log-model COMMAND log-model-test)
  add_executable(connection-state-test tests/connection_state_test.c src/connection_state.c)
  target_include_directories(connection-state-test PRIVATE src)
  target_link_libraries(connection-state-test PRIVATE PkgConfig::GTK4)
  add_test(NAME connection-state COMMAND connection-state-test)
  add_library(serial-ioctl-shim SHARED tests/serial_ioctl_shim.c)
  add_executable(serial-options-test tests/serial_options_test.c src/serial_options.c src/settings.c)
  target_include_directories(serial-options-test PRIVATE src)
  target_link_libraries(serial-options-test PRIVATE PkgConfig::GTK4)
  add_test(NAME serial-options COMMAND serial-options-test)
  add_executable(sequence-test tests/sequence_test.c src/sequence.c src/payload.c)
  target_include_directories(sequence-test PRIVATE src)
  target_link_libraries(sequence-test PRIVATE PkgConfig::GTK4)
  add_test(NAME sequence COMMAND sequence-test)
  add_executable(quick-presets-test tests/quick_presets_test.c src/quick_presets.c src/payload.c src/settings.c)
  target_include_directories(quick-presets-test PRIVATE src)
  target_link_libraries(quick-presets-test PRIVATE PkgConfig::GTK4)
  add_test(NAME quick-presets COMMAND quick-presets-test)
  add_executable(payload-test tests/payload_test.c src/payload.c)
  target_include_directories(payload-test PRIVATE src)
  target_link_libraries(payload-test PRIVATE PkgConfig::GTK4)
  add_test(NAME payload COMMAND payload-test)
  add_executable(highlighter-test
    tests/highlighter_test.c
    src/highlighter.c
  )
  target_include_directories(highlighter-test PRIVATE src)
  target_link_libraries(highlighter-test PRIVATE PkgConfig::GTK4)
  target_compile_options(highlighter-test PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion>
  )
  add_test(NAME highlighter COMMAND highlighter-test)
endif()
target_link_libraries(tio-gui PRIVATE
  PkgConfig::GTK4
  PkgConfig::VTE
  PkgConfig::PCRE2
  PkgConfig::SOUP
  PkgConfig::JSON_GLIB
)
target_compile_definitions(tio-gui PRIVATE
  TIO_GUI_VERSION="${PROJECT_VERSION}"
  TIO_GUI_LOCALE_DIR="${CMAKE_INSTALL_FULL_LOCALEDIR}"
  TIO_GUI_BUILD_LOCALE_DIR="${CMAKE_CURRENT_BINARY_DIR}/locale"
)
target_compile_options(tio-gui PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion>
)

if(BUILD_TESTING)
  add_executable(scroll-test tests/scroll_test.c src/settings.c src/highlighter.c src/payload.c src/quick_presets.c src/sequence.c src/serial_options.c src/connection_state.c src/log_model.c src/analyzer.c src/capture.c src/transfer.c src/plugin.c src/plugin_ui.c src/network.c src/network_ui.c src/modbus.c src/modbus_ui.c src/mqtt.c src/mqtt_ui.c)
  target_link_libraries(scroll-test PRIVATE PkgConfig::GTK4 PkgConfig::VTE
    PkgConfig::PCRE2 PkgConfig::SOUP PkgConfig::JSON_GLIB)
  target_compile_definitions(scroll-test PRIVATE
    TIO_GUI_VERSION="${PROJECT_VERSION}"
    TIO_GUI_LOCALE_DIR="${CMAKE_INSTALL_FULL_LOCALEDIR}"
    TIO_GUI_BUILD_LOCALE_DIR="${CMAKE_CURRENT_BINARY_DIR}/locale")
  # Requires a display: run manually in a desktop session or under Xvfb.
  add_executable(keyboard-test tests/keyboard_test.c src/settings.c src/highlighter.c src/payload.c src/quick_presets.c src/sequence.c src/serial_options.c src/connection_state.c src/log_model.c src/analyzer.c src/capture.c src/transfer.c src/plugin.c src/plugin_ui.c src/network.c src/network_ui.c src/modbus.c src/modbus_ui.c src/mqtt.c src/mqtt_ui.c)
  target_link_libraries(keyboard-test PRIVATE PkgConfig::GTK4 PkgConfig::VTE
    PkgConfig::PCRE2 PkgConfig::SOUP PkgConfig::JSON_GLIB)
  target_compile_definitions(keyboard-test PRIVATE
    TIO_GUI_VERSION="${PROJECT_VERSION}"
    TIO_GUI_LOCALE_DIR="${CMAKE_INSTALL_FULL_LOCALEDIR}"
    TIO_GUI_BUILD_LOCALE_DIR="${CMAKE_CURRENT_BINARY_DIR}/locale")
  add_executable(quick-editor-test tests/quick_editor_test.c src/settings.c src/highlighter.c src/payload.c src/quick_presets.c src/sequence.c src/serial_options.c src/connection_state.c src/log_model.c src/analyzer.c src/capture.c src/transfer.c src/plugin.c src/plugin_ui.c src/network.c src/network_ui.c src/modbus.c src/modbus_ui.c src/mqtt.c src/mqtt_ui.c)
  target_link_libraries(quick-editor-test PRIVATE PkgConfig::GTK4 PkgConfig::VTE
    PkgConfig::PCRE2 PkgConfig::SOUP PkgConfig::JSON_GLIB)
  target_compile_definitions(quick-editor-test PRIVATE
    TIO_GUI_VERSION="${PROJECT_VERSION}"
    TIO_GUI_LOCALE_DIR="${CMAKE_INSTALL_FULL_LOCALEDIR}"
    TIO_GUI_BUILD_LOCALE_DIR="${CMAKE_CURRENT_BINARY_DIR}/locale")
endif()

install(TARGETS tio-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/io.github.keithxc.tio_gui.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

foreach(TIO_GUI_ICON_SIZE 16 32 48 64 128 256 512)
  install(
    FILES
      data/icons/hicolor/${TIO_GUI_ICON_SIZE}x${TIO_GUI_ICON_SIZE}/apps/io.github.keithxc.tio_gui.png
    DESTINATION
      ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${TIO_GUI_ICON_SIZE}x${TIO_GUI_ICON_SIZE}/apps
  )
endforeach()

set(TIO_GUI_TRANSLATIONS zh_CN zh_TW ja de)
set(TIO_GUI_MO_FILES)
foreach(TIO_GUI_LANGUAGE IN LISTS TIO_GUI_TRANSLATIONS)
  set(TIO_GUI_MO
      "${CMAKE_CURRENT_BINARY_DIR}/locale/${TIO_GUI_LANGUAGE}/LC_MESSAGES/tio-gui.mo")
  add_custom_command(
    OUTPUT "${TIO_GUI_MO}"
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/locale/${TIO_GUI_LANGUAGE}/LC_MESSAGES"
    COMMAND ${GETTEXT_MSGFMT_EXECUTABLE}
            --check --output-file=${TIO_GUI_MO}
            "${CMAKE_CURRENT_SOURCE_DIR}/po/${TIO_GUI_LANGUAGE}.po"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/po/${TIO_GUI_LANGUAGE}.po"
    VERBATIM
  )
  list(APPEND TIO_GUI_MO_FILES "${TIO_GUI_MO}")
  install(FILES "${TIO_GUI_MO}"
          DESTINATION "${CMAKE_INSTALL_LOCALEDIR}/${TIO_GUI_LANGUAGE}/LC_MESSAGES")
endforeach()
add_custom_target(translations ALL DEPENDS ${TIO_GUI_MO_FILES})

# Not part of ALL: run `cmake --build build --target update-pot` after adding or
# changing translatable strings, then merge po/tio-gui.pot into each catalogue.
find_program(TIO_GUI_XGETTEXT xgettext)
if(TIO_GUI_XGETTEXT)
  add_custom_target(update-pot
    COMMAND ${TIO_GUI_XGETTEXT}
            --from-code=UTF-8 --language=C --keyword=_ --keyword=ngettext:1,2
            --package-name=tio-gui --package-version=${PROJECT_VERSION}
            --copyright-holder="tio-gui contributors"
            --msgid-bugs-address=https://github.com/keithxc/tio-gui/issues
            --no-location
            -o "${CMAKE_CURRENT_SOURCE_DIR}/po/tio-gui.pot"
            src/main.c src/settings.c src/analyzer.c
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Regenerating po/tio-gui.pot"
    VERBATIM
  )
endif()
