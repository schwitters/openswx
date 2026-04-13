#!/usr/bin/env bash
# run_regression.sh — Batch-Regressionstest: swx_dump vs. swdm_dump
#
# Verwendung:
#   ./run_regression.sh <SW-Verzeichnis> <DM-JSON-Verzeichnis>
#
# Ablauf:
#   1. Für jede .SLDPRT/.SLDASM/.SLDDRW in <SW-Verzeichnis>:
#      - swx_dump ausführen → <basename>.openswx.json
#   2. Jede .openswx.json mit der gleichnamigen .dm.json aus <DM-JSON-Verzeichnis> vergleichen
#      (Die .dm.json-Dateien werden vorab auf Windows mit swdm_dump.exe erzeugt)
#   3. Zusammenfassung am Ende
#
# Voraussetzung:
#   swx_dump muss gebaut sein: sw_dumper/build/asmbox/swx_dump
#   Python 3.x muss verfügbar sein

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SW_DUMPER_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SWX_DUMP="$SW_DUMPER_DIR/build/asmbox/swx_dump"
COMPARE_PY="$SCRIPT_DIR/compare_dumps.py"

if [[ $# -lt 2 ]]; then
    echo "Verwendung: $0 <SW-Verzeichnis> <DM-JSON-Verzeichnis> [--quiet]"
    echo ""
    echo "  <SW-Verzeichnis>      Verzeichnis mit .SLDPRT/.SLDASM/.SLDDRW Dateien"
    echo "  <DM-JSON-Verzeichnis> Verzeichnis mit *.dm.json (erzeugt von swdm_dump.exe auf Windows)"
    echo "  --quiet               Nur Zusammenfassung ausgeben"
    exit 1
fi

SLD_DIR="$1"
DM_DIR="$2"
QUIET=""
if [[ "${3:-}" == "--quiet" ]]; then
    QUIET="-q"
fi

if [[ ! -x "$SWX_DUMP" ]]; then
    echo "Fehler: swx_dump nicht gefunden oder nicht ausführbar: $SWX_DUMP"
    echo "Bitte zuerst bauen: cmake --build sw_dumper/build --target swx_dump"
    exit 1
fi

if [[ ! -f "$COMPARE_PY" ]]; then
    echo "Fehler: compare_dumps.py nicht gefunden: $COMPARE_PY"
    exit 1
fi

# Temporäres Verzeichnis für openswx-Dumps
TMPDIR_OPENSWX="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_OPENSWX"' EXIT

echo "=== Regression Test: swdm_dump vs. swx_dump ==="
echo "SW-Dateien : $SLD_DIR"
echo "DM-JSONs   : $DM_DIR"
echo "swx_dump   : $SWX_DUMP"
echo ""

PASS=0
FAIL=0
SKIP=0
ERRORS=0

# Alle SolidWorks-Dateien finden (rekursiv, case-insensitive)
mapfile -d '' SW_FILES < <(
    find "$SLD_DIR" -type f \( \
        -iname "*.sldprt" -o \
        -iname "*.sldasm" -o \
        -iname "*.slddrw" \
    \) -print0 | sort -z
)

TOTAL=${#SW_FILES[@]}
echo "Gefundene Dateien: $TOTAL"
echo ""

for sw_file in "${SW_FILES[@]}"; do
    base="$(basename "$sw_file")"
    stem="${base%.*}"          # Dateiname ohne Extension
    stem_upper="${stem^^}"     # Großschreibung für Windows-Vergleich

    # Suche nach DM-JSON (case-insensitive, da Windows-Dateinamen)
    dm_json=""
    for candidate in "$DM_DIR/${stem}.dm.json" "$DM_DIR/${stem_upper}.dm.json"; do
        if [[ -f "$candidate" ]]; then
            dm_json="$candidate"
            break
        fi
    done
    # Auch ohne .dm-Suffix suchen (nur <stem>.json)
    if [[ -z "$dm_json" ]]; then
        for candidate in "$DM_DIR/${stem}.json" "$DM_DIR/${stem_upper}.json"; do
            if [[ -f "$candidate" ]]; then
                dm_json="$candidate"
                break
            fi
        done
    fi

    if [[ -z "$dm_json" ]]; then
        echo "  SKIP $base  (kein DM-JSON in $DM_DIR)"
        (( SKIP++ )) || true
        continue
    fi

    # swx_dump ausführen
    openswx_json="$TMPDIR_OPENSWX/${stem}.openswx.json"
    if ! "$SWX_DUMP" "$sw_file" > "$openswx_json" 2>/dev/null; then
        echo "  ERR  $base  (swx_dump fehlgeschlagen)"
        (( ERRORS++ )) || true
        continue
    fi

    # Vergleichen
    if python3 "$COMPARE_PY" $QUIET "$dm_json" "$openswx_json"; then
        (( PASS++ )) || true
    else
        (( FAIL++ )) || true
    fi
done

echo ""
echo "=== Ergebnis ==="
echo "  Gesamt : $TOTAL"
echo "  OK     : $PASS"
echo "  FAIL   : $FAIL"
echo "  SKIP   : $SKIP  (kein DM-JSON vorhanden)"
echo "  ERROR  : $ERRORS  (swx_dump fehlgeschlagen)"
echo ""

if [[ $FAIL -gt 0 || $ERRORS -gt 0 ]]; then
    exit 1
fi
exit 0
