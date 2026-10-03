# The serial transport can run without GTK, a display server, or tio/VTE.
include(GNUInstallDirs)
include(CTest)
find_package(PkgConfig REQUIRED)
pkg_check_modules(AGENT_GIO REQUIRED IMPORTED_TARGET gio-2.0>=2.66)
pkg_check_modules(AGENT_JSON REQUIRED IMPORTED_TARGET json-glib-1.0>=1.6)
add_executable(tio-serial-agent src/serial_agent.c src/native_serial.c)
target_link_libraries(tio-serial-agent PRIVATE PkgConfig::AGENT_GIO PkgConfig::AGENT_JSON)
target_compile_definitions(tio-serial-agent PRIVATE TIO_GUI_VERSION="${PROJECT_VERSION}")
target_compile_options(tio-serial-agent PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)
if(WIN32)
  target_link_libraries(tio-serial-agent PRIVATE advapi32)
elseif(APPLE)
  target_link_libraries(tio-serial-agent PRIVATE "-framework IOKit" "-framework CoreFoundation")
endif()
install(TARGETS tio-serial-agent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY remote/ DESTINATION ${CMAKE_INSTALL_DATADIR}/tio-gui/remote
  PATTERN "__pycache__" EXCLUDE PATTERN "._*" EXCLUDE)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/tio-gui)
install(FILES docs/remote-serial.md docs/cross-platform.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/tio-gui)
if(BUILD_TESTING)
  set(Python3_FIND_FRAMEWORK LAST)
  find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  add_test(NAME serial-agent COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/serial_agent_test.py $<TARGET_FILE:tio-serial-agent>)
  set_tests_properties(serial-agent PROPERTIES TIMEOUT 40)
  add_test(NAME remote-gateway COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/remote_gateway_test.py)
  add_test(NAME remote-serial-integration COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/remote_serial_integration_test.py $<TARGET_FILE:tio-serial-agent>)
  set_tests_properties(remote-gateway remote-serial-integration PROPERTIES TIMEOUT 60)
endif()
