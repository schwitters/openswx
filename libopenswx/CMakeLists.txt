# SPDX-License-Identifier: MIT
# Copyright (c) 2026 openswx contributors

cmake_minimum_required(VERSION 3.20)
project(openswx VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ZLIB REQUIRED)
find_package(pugixml CONFIG REQUIRED)

add_library(openswx STATIC
  src/document.cc
  src/internal/decompressor.cc
  src/internal/modern_parser.cc
  src/internal/ole2_parser.cc
  src/internal/xml_readers.cc
  src/internal/sheet_name_reader.cc
)

target_include_directories(openswx
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(openswx
  PRIVATE
    ZLIB::ZLIB
    pugixml::pugixml
)

target_compile_options(openswx PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow>
  $<$<CXX_COMPILER_ID:MSVC>:
    /W4 /permissive- /utf-8>
)

# Optional: install targets for use as a sub-project.
install(TARGETS openswx EXPORT openswxTargets ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
