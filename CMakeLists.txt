cmake_minimum_required(VERSION 3.27)

file(READ "${CMAKE_SOURCE_DIR}/VERSION" VER_RAW)
string(STRIP ${VER_RAW} VERSION)

project(hyprdictate VERSION ${VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-value)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
find_package(hyprwayland-scanner 0.4.0 REQUIRED)
find_program(HYPRWIRE_SCANNER hyprwire-scanner REQUIRED)

pkg_check_modules(deps REQUIRED IMPORTED_TARGET wayland-client libpipewire-0.3 hyprwire hyprutils)

# transcribe.cpp, built and installed on its own (nix/transcribe-cpp.nix): its
# CMake isn't usable as a subdirectory of another project.
find_path(TRANSCRIBE_INCLUDE_DIR transcribe.h REQUIRED)
find_library(TRANSCRIBE_LIBRARY transcribe REQUIRED)

add_executable(hyprdictate src/main.cpp src/Audio.cpp src/InputMethod.cpp src/Transcriber.cpp)

set(GENERATED ${CMAKE_CURRENT_BINARY_DIR}/protocols)
file(MAKE_DIRECTORY ${GENERATED})

add_custom_command(
  OUTPUT ${GENERATED}/input-method-unstable-v2.cpp ${GENERATED}/input-method-unstable-v2.hpp
  COMMAND hyprwayland-scanner --client ${CMAKE_SOURCE_DIR}/protocols/input-method-unstable-v2.xml ${GENERATED}/
  DEPENDS ${CMAKE_SOURCE_DIR}/protocols/input-method-unstable-v2.xml
  VERBATIM)

foreach(side server client)
  if(side STREQUAL client)
    set(flag --client)
  else()
    set(flag)
  endif()
  add_custom_command(
    OUTPUT ${GENERATED}/hyprdictate_core-${side}.cpp ${GENERATED}/hyprdictate_core-${side}.hpp
    COMMAND ${HYPRWIRE_SCANNER} ${flag} ${CMAKE_SOURCE_DIR}/protocols/hyprdictate_core.xml ${GENERATED}/
    DEPENDS ${CMAKE_SOURCE_DIR}/protocols/hyprdictate_core.xml
    VERBATIM)
  target_sources(hyprdictate PRIVATE ${GENERATED}/hyprdictate_core-${side}.cpp)
endforeach()

target_sources(hyprdictate PRIVATE ${GENERATED}/input-method-unstable-v2.cpp)
target_include_directories(hyprdictate PRIVATE ${GENERATED} ${TRANSCRIBE_INCLUDE_DIR})
target_link_libraries(hyprdictate PRIVATE PkgConfig::deps ${TRANSCRIBE_LIBRARY})

install(TARGETS hyprdictate)
