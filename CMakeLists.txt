cmake_minimum_required(VERSION 3.20)

project(
  tio-gui
  VERSION 0.1.0
  DESCRIPTION "A lightweight, reliable GUI for tio"
  LANGUAGES C
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.12)
pkg_check_modules(VTE REQUIRED IMPORTED_TARGET vte-2.91-gtk4>=0.74)

add_executable(tio-gui src/main.c)
target_link_libraries(tio-gui PRIVATE PkgConfig::GTK4 PkgConfig::VTE)
target_compile_options(tio-gui PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion>
)

install(TARGETS tio-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
