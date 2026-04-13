#!/usr/bin/env bash
# run_regression_all.sh — Vergleicht swdm_dump- und swx_dump-Ausgaben für alle
# SW-Dateien in files/, files2/ und files-w-cutlists/.
#
# Voraussetzung:
#   - dump_all.cmd auf Windows ausgeführt → *.dm.json neben den SW-Dateien
#   - swx_dump gebaut: sw_dumper/build/asmbox/swx_dump
#
# Verwendung:
#   ./run_regression_all.sh [--quiet]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SWX_DUMP="$SCRIPT_DIR/../build/asmbox/swx_dump"
COMPARE_PY="$SCRIPT_DIR/compare_dumps.py"
QUIET=""
[[ "${1:-}" == "--quiet" ]] && QUIET="-q"

if [[ ! -x "$SWX_DUMP" ]]; then
    echo "Fehler: swx_dump nicht gefunden: $SWX_DUMP" >&2
    exit 1
fi

TMPDIR_SWX="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_SWX"' EXIT

PASS=0; FAIL=0; SKIP=0; ERR=0

for dir in files files2 files-w-cutlists; do
    full="$REPO_ROOT/$dir"
    [[ -d "$full" ]] || { echo "WARNUNG: $full nicht gefunden"; continue; }

    echo ""
    echo "=== $dir ==="

    while IFS= read -r -d '' sw_file; do
        base="$(basename "$sw_file")"
        dm_json="${sw_file}.dm.json"

        if [[ ! -f "$dm_json" ]]; then
            echo "  SKIP $base  (kein .dm.json)"
            (( SKIP++ )) || true
            continue
        fi

        swx_json="$TMPDIR_SWX/${base}.swx.json"
        if ! "$SWX_DUMP" "$sw_file" > "$swx_json" 2>/dev/null; then
            echo "  ERR  $base  (swx_dump fehlgeschlagen)"
            (( ERR++ )) || true
            continue
        fi

        if python3 "$COMPARE_PY" $QUIET "$dm_json" "$swx_json"; then
            (( PASS++ )) || true
        else
            (( FAIL++ )) || true
        fi
    done < <(find "$full" -maxdepth 1 -type f \( \
        -iname "*.sldprt" -o -iname "*.sldasm" -o -iname "*.slddrw" \
    \) -print0 | sort -z)
done

echo ""
echo "=== Ergebnis ==="
echo "  OK   : $PASS"
echo "  FAIL : $FAIL"
echo "  SKIP : $SKIP  (kein .dm.json)"
echo "  ERR  : $ERR   (swx_dump fehlgeschlagen)"

[[ $FAIL -eq 0 && $ERR -eq 0 ]] && exit 0 || exit 1
