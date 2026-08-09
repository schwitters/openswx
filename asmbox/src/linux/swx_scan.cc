// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "swx_scan/scanner.h"

namespace fs = std::filesystem;

namespace {

void PrintUsage() {
  std::cerr << "Usage: swx_scan [--report report.json] <directory>\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string report_path;
  std::string scan_root;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--report") {
      if (i + 1 >= argc) {
        PrintUsage();
        return 1;
      }
      report_path = argv[++i];
      continue;
    }
    if (!scan_root.empty()) {
      PrintUsage();
      return 1;
    }
    scan_root = arg;
  }

  if (scan_root.empty()) {
    PrintUsage();
    return 1;
  }

  const auto report_result =
      sw_dumper::scan::ScanDirectory(scan_root, sw_dumper::scan::ParseWithOpenswx);
  if (!report_result.ok()) {
    std::cerr << "Error: " << report_result.error() << "\n";
    return 1;
  }

  const sw_dumper::scan::ScanReport& report = report_result.value();
  std::cout << sw_dumper::scan::FormatConsoleReport(report);

  if (!report_path.empty()) {
    std::ofstream output(report_path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      std::cerr << "Error: Cannot write report: " << report_path << "\n";
      return 1;
    }
    output << sw_dumper::scan::ScanReportToJson(report).dump(2) << "\n";
    if (!output) {
      std::cerr << "Error: Failed while writing report: " << report_path
                << "\n";
      return 1;
    }
  }

  return 0;
}
