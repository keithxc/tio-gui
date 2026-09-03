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

add_executable(tio-gui
  src/main.c
  src/settings.c
)
target_link_libraries(tio-gui PRIVATE PkgConfig::GTK4 PkgConfig::VTE)
target_compile_definitions(tio-gui PRIVATE
  TIO_GUI_VERSION="${PROJECT_VERSION}"
  TIO_GUI_LOCALE_DIR="${CMAKE_INSTALL_FULL_LOCALEDIR}"
  TIO_GUI_BUILD_LOCALE_DIR="${CMAKE_CURRENT_BINARY_DIR}/locale"
)
target_compile_options(tio-gui PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion>
)

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
