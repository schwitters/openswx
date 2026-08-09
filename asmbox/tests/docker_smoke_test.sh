#!/bin/sh
set -eu

IMAGE_TAG="${1:-openswx:test}"
REPO_ROOT="${2:-.}"
CONTAINER_BIN="${CONTAINER_BIN:-docker}"
HOST_PORT="${HOST_PORT:-18087}"

if ! "${CONTAINER_BIN}" version >/tmp/openswx_container_version.out 2>&1; then
  echo "[  SKIPPED ] container runtime is unavailable"
  cat /tmp/openswx_container_version.out
  exit 0
fi

cleanup() {
  if [ -n "${ASMB0X_CID:-}" ]; then
    "${CONTAINER_BIN}" rm -f "${ASMB0X_CID}" >/dev/null 2>&1 || true
  fi
}

trap cleanup EXIT

"${CONTAINER_BIN}" build -t "${IMAGE_TAG}" "${REPO_ROOT}"

"${CONTAINER_BIN}" run --rm --entrypoint swx_dump "${IMAGE_TAG}" \
  >/tmp/openswx_swx_dump.out 2>&1 || true
if ! grep -q "Usage: swx_dump <SolidWorks file>" /tmp/openswx_swx_dump.out; then
  echo "swx_dump usage output not detected"
  cat /tmp/openswx_swx_dump.out
  exit 1
fi

ASMB0X_CID="$("${CONTAINER_BIN}" run -d -p "${HOST_PORT}:8087" "${IMAGE_TAG}")"
sleep 2

"${CONTAINER_BIN}" exec "${ASMB0X_CID}" sh -c 'test "$(id -u)" != "0"'

curl -fsS "http://127.0.0.1:${HOST_PORT}/" >/tmp/openswx_asmbox.out
grep -q "<!DOCTYPE html>" /tmp/openswx_asmbox.out
