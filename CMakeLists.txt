# SPDX-License-Identifier: MIT
# Copyright (c) 2026 openswx contributors

cmake_minimum_required(VERSION 3.20)
project(SwDumper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)

if(BUILD_TESTING AND NOT TARGET GTest::gtest_main)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

# ── libopenswx — platform-independent SolidWorks file parser ─────────────────
add_subdirectory(libopenswx)

# ── libopenbom — configurable BOM transformer built on libopenswx ─────────────
add_subdirectory(libopenbom)

# ── asmbox — Linux HTTP server (references libopenswx) ───────────────────────
add_subdirectory(asmbox)

# ── Windows-only server binary ────────────────────────────────────────────────
if(WIN32)
  add_compile_definitions(_WIN32_WINNT=0x0A00)
  add_compile_options(/W4 /permissive- /utf-8)
  add_compile_definitions(UNICODE _UNICODE)

  find_package(nlohmann_json CONFIG REQUIRED)
  find_package(wil            CONFIG REQUIRED)
  find_package(plog           CONFIG REQUIRED)
  find_package(Crow           CONFIG REQUIRED)
  find_package(libzip         CONFIG REQUIRED)
  find_package(unofficial-sqlite3 CONFIG REQUIRED)

  set(IDL_INPUT "${CMAKE_CURRENT_SOURCE_DIR}/src/idl/SwDocumentMgr.idl")
  set(MIDL_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
  file(MAKE_DIRECTORY ${MIDL_OUT_DIR})
  add_custom_command(
    OUTPUT  "${MIDL_OUT_DIR}/SwDocumentMgr.h"
            "${MIDL_OUT_DIR}/SwDocumentMgr_i.c"
    COMMAND midl /h "SwDocumentMgr.h" /iid "SwDocumentMgr_i.c"
                 /out "${MIDL_OUT_DIR}" "${IDL_INPUT}"
    DEPENDS ${IDL_INPUT}
  )

  add_executable(SwServer
    src/main.cc
    src/server.cc
    src/sw/document_analyzer.cc
    src/storage/sqlite_storage.cc
    src/utils/html_exporter.cc
    src/utils/string_utils.cc
    "${MIDL_OUT_DIR}/SwDocumentMgr_i.c"
  )
  target_compile_definitions(SwServer
    PRIVATE SW_LICENSE_KEY=L"${SW_LICENSE_KEY}"
  )
  target_include_directories(SwServer PRIVATE src ${MIDL_OUT_DIR})
  target_link_libraries(SwServer
    PRIVATE
      openswx
      nlohmann_json::nlohmann_json
      WIL::WIL
      plog::plog
      Crow::Crow
      libzip::zip
      unofficial::sqlite3::sqlite3
      ole32 oleaut32
  )
  add_custom_command(TARGET SwServer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/templates"
            "${CMAKE_CURRENT_BINARY_DIR}/templates"
  )
endif()
