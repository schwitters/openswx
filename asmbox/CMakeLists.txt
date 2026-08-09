# SPDX-License-Identifier: MIT
# Copyright (c) 2026 openswx contributors

cmake_minimum_required(VERSION 3.20)
project(asmbox LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── libopenswx + libopenbom ───────────────────────────────────────────────────
# When built as part of the top-level project these targets are already defined.
# When asmbox is configured stand-alone we build them here.
if(NOT TARGET openswx)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../libopenswx
                   ${CMAKE_CURRENT_BINARY_DIR}/openswx-build)
endif()
if(NOT TARGET openbom)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../libopenbom
                   ${CMAKE_CURRENT_BINARY_DIR}/openbom-build)
endif()

# ── System packages ────────────────────────────────────────────────────────────
find_package(nlohmann_json CONFIG REQUIRED)
find_package(SQLite3       REQUIRED)
find_package(libzip        CONFIG REQUIRED)

# ── Crow (header-only) via FetchContent ────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
  crow
  GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
  GIT_TAG        v1.3.3
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(crow)

# ── plog (header-only) via FetchContent ───────────────────────────────────────
FetchContent_Declare(
  plog
  GIT_REPOSITORY https://github.com/SergiusTheBest/plog.git
  GIT_TAG        1.1.10
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(plog)

# ── asmbox server ─────────────────────────────────────────────────────────────
add_executable(asmbox
  src/server.cc
  src/sw/document_analyzer.cc
  src/storage/sqlite_storage.cc
  src/utils/string_utils.cc
  src/utils/zip_extractor.cc
)
target_include_directories(asmbox PRIVATE src)
# Pull third-party headers in as SYSTEM includes so our -Wall/-Wconversion
# flags don't fire on Crow's or plog's code.
target_include_directories(asmbox SYSTEM PRIVATE
  $<TARGET_PROPERTY:Crow::Crow,INTERFACE_INCLUDE_DIRECTORIES>
  $<TARGET_PROPERTY:plog::plog,INTERFACE_INCLUDE_DIRECTORIES>
)
target_link_libraries(asmbox PRIVATE
  openbom
  openswx
  nlohmann_json::nlohmann_json
  Crow::Crow
  plog::plog
  SQLite::SQLite3
  libzip::zip
)
target_compile_options(asmbox PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion -Wshadow>
)
# Copy templates next to the binary so Crow's mustache loader finds them.
add_custom_command(TARGET asmbox POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/templates
          $<TARGET_FILE_DIR:asmbox>/templates
)

# ── swx_dump CLI ──────────────────────────────────────────────────────────────
add_executable(swx_dump src/linux/swx_dump.cc)
target_link_libraries(swx_dump PRIVATE openswx nlohmann_json::nlohmann_json)

add_executable(swx_scan
  src/linux/swx_scan.cc
  src/swx_scan/scanner.cc
)
target_include_directories(swx_scan PRIVATE src)
target_link_libraries(swx_scan PRIVATE
  openswx
  nlohmann_json::nlohmann_json
)

if(BUILD_TESTING)
  add_executable(asmbox_tests
    tests/swx_scan_test.cc
    tests/utils_test.cc
    tests/zip_extractor_test.cc
    src/swx_scan/scanner.cc
    src/utils/string_utils.cc
    src/utils/zip_extractor.cc
  )
  target_include_directories(asmbox_tests PRIVATE src)
  target_link_libraries(asmbox_tests PRIVATE
    GTest::gtest_main
    nlohmann_json::nlohmann_json
    openswx
    plog::plog
    libzip::zip
  )
  include(GoogleTest)
  gtest_discover_tests(asmbox_tests)
endif()
