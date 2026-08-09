#!/bin/sh
set -eu

IMAGE_TAG="${1:-openswx:test}"
REPO_ROOT="${2:-.}"
CONTAINER_BIN="${CONTAINER_BIN:-docker}"
HOST_PORT="${HOST_PORT:-18087}"

if [ "${CONTAINER_BIN}" = "podman" ]; then
  export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/podman-runtime}"
  export CONTAINERS_STORAGE_CONF="${CONTAINERS_STORAGE_CONF:-}"
  mkdir -p "${XDG_RUNTIME_DIR}" /tmp/podman-root /tmp/podman-runroot
  CONTAINER_CMD="${CONTAINER_BIN} --root /tmp/podman-root --runroot /tmp/podman-runroot"
else
  CONTAINER_CMD="${CONTAINER_BIN}"
fi

if ! sh -c "${CONTAINER_CMD} version" >/tmp/openswx_container_version.out 2>&1; then
  echo "[  SKIPPED ] container runtime is unavailable"
  cat /tmp/openswx_container_version.out
  exit 0
fi

cleanup() {
  if [ -n "${ASMB0X_CID:-}" ]; then
    sh -c "${CONTAINER_CMD} rm -f \"${ASMB0X_CID}\"" >/dev/null 2>&1 || true
  fi
}

trap cleanup EXIT

sh -c "${CONTAINER_CMD} build -t \"${IMAGE_TAG}\" \"${REPO_ROOT}\""

sh -c "${CONTAINER_CMD} run --rm --entrypoint swx_dump \"${IMAGE_TAG}\"" \
  >/tmp/openswx_swx_dump.out 2>&1 || true
if ! grep -q "Usage: swx_dump <SolidWorks file>" /tmp/openswx_swx_dump.out; then
  echo "swx_dump usage output not detected"
  cat /tmp/openswx_swx_dump.out
  exit 1
fi

ASMB0X_CID="$(sh -c "${CONTAINER_CMD} run -d -p ${HOST_PORT}:8087 \"${IMAGE_TAG}\"")"
sleep 2

sh -c "${CONTAINER_CMD} exec \"${ASMB0X_CID}\" sh -c 'test \"\$(id -u)\" != \"0\"'"

curl -fsS "http://127.0.0.1:${HOST_PORT}/" >/tmp/openswx_asmbox.out
grep -q "<!DOCTYPE html>" /tmp/openswx_asmbox.out
