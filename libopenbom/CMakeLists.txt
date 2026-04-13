# SPDX-License-Identifier: MIT
# Copyright (c) 2026 openswx contributors

cmake_minimum_required(VERSION 3.20)
project(openbom VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── libopenswx — bring in the SolidWorks parser ───────────────────────────────
if(NOT TARGET openswx)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../libopenswx
                   ${CMAKE_CURRENT_BINARY_DIR}/openswx-build)
endif()

# ── nlohmann/json — JSON serialisation ───────────────────────────────────────
find_package(nlohmann_json CONFIG REQUIRED)

# ── openbom static library ───────────────────────────────────────────────────
add_library(openbom STATIC
  src/path_resolver.cc
  src/transformer.cc
  src/json_writer.cc
)

target_include_directories(openbom
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(openbom
  PUBLIC
    openswx                        # Result<T> and openswx types appear in the public API
  PRIVATE
    nlohmann_json::nlohmann_json   # used only inside json_writer.cc
)

target_compile_options(openbom PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow>
  $<$<CXX_COMPILER_ID:MSVC>:
    /W4 /permissive- /utf-8>
)

# ── Optional: install ─────────────────────────────────────────────────────────
install(TARGETS openbom EXPORT openbomTargets ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
