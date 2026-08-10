#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "Usage: scripts/local_scan_report.sh <input-path> <output-dir> [swx-binary-dir]" >&2
  exit 1
fi

INPUT_PATH=$1
OUTPUT_DIR=$2
SWX_BIN_DIR=${3:-build/asmbox}

SWX_DUMP="${SWX_BIN_DIR}/swx_dump"
SWX_SCAN="${SWX_BIN_DIR}/swx_scan"

if [[ ! -e "${INPUT_PATH}" ]]; then
  echo "Input path does not exist: ${INPUT_PATH}" >&2
  exit 1
fi

if [[ ! -x "${SWX_DUMP}" ]]; then
  echo "swx_dump is not executable: ${SWX_DUMP}" >&2
  exit 1
fi

if [[ ! -x "${SWX_SCAN}" ]]; then
  echo "swx_scan is not executable: ${SWX_SCAN}" >&2
  exit 1
fi

mkdir -p "${OUTPUT_DIR}"
FILES_DIR="${OUTPUT_DIR}/files"
mkdir -p "${FILES_DIR}"

SUMMARY_MD="${OUTPUT_DIR}/summary.md"
SCAN_TXT="${OUTPUT_DIR}/scan.txt"
SCAN_JSON="${OUTPUT_DIR}/scan.json"
MANIFEST_TXT="${OUTPUT_DIR}/manifest.txt"

find_input_files() {
  if [[ -d "${INPUT_PATH}" ]]; then
    find "${INPUT_PATH}" -type f \( -iname '*.sldprt' -o -iname '*.sldasm' -o -iname '*.slddrw' \) | sort
  else
    printf '%s\n' "${INPUT_PATH}"
  fi
}

sanitize_basename() {
  local raw_name=$1
  raw_name=${raw_name##*/}
  raw_name=${raw_name// /_}
  raw_name=${raw_name//[^A-Za-z0-9._-]/_}
  printf '%s' "${raw_name}"
}

relative_display_path() {
  local file_path=$1
  if [[ -d "${INPUT_PATH}" ]]; then
    python3 -c 'import os, sys; print(os.path.relpath(sys.argv[2], sys.argv[1]))' \
      "${INPUT_PATH}" "${file_path}"
  else
    basename "${file_path}"
  fi
}

"${SWX_SCAN}" --report "${SCAN_JSON}" "${INPUT_PATH}" > "${SCAN_TXT}"

find_input_files > "${MANIFEST_TXT}"

{
  echo "# Local SolidWorks Scan Report"
  echo
  echo "- Input path: \`${INPUT_PATH}\`"
  echo "- Output directory: \`${OUTPUT_DIR}\`"
  echo "- Generated at: \`$(date -Iseconds)\`"
  echo
  echo "## Aggregate Scan"
  echo
  echo '```text'
  cat "${SCAN_TXT}"
  echo '```'
  echo
  echo "## Per-file Dumps"
  echo
} > "${SUMMARY_MD}"

while IFS= read -r solidworks_file; do
  [[ -n "${solidworks_file}" ]] || continue
  display_path=$(relative_display_path "${solidworks_file}")
  safe_name=$(sanitize_basename "${display_path}")
  json_output="${FILES_DIR}/${safe_name}.json"
  err_output="${FILES_DIR}/${safe_name}.stderr.txt"

  if "${SWX_DUMP}" "${solidworks_file}" > "${json_output}" 2> "${err_output}"; then
    {
      echo "- \`${display_path}\`"
      echo "  Output: [${safe_name}.json](files/${safe_name}.json)"
    } >> "${SUMMARY_MD}"
    rm -f "${err_output}"
  else
    {
      echo "- \`${display_path}\`"
      echo "  Error: [${safe_name}.stderr.txt](files/${safe_name}.stderr.txt)"
    } >> "${SUMMARY_MD}"
  fi
done < "${MANIFEST_TXT}"

echo >> "${SUMMARY_MD}"
echo "## Machine-readable Aggregate Report" >> "${SUMMARY_MD}"
echo >> "${SUMMARY_MD}"
echo "- [scan.json](scan.json)" >> "${SUMMARY_MD}"
