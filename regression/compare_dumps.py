#!/usr/bin/env python3
"""
compare_dumps.py  —  Vergleicht JSON-Dumps von swdm_dump (Document Manager, Windows)
und swx_dump (libopenswx, Linux) für Regressionstests.

Verwendung (einzelne Datei):
    python3 compare_dumps.py <dm_dump.json> <swx_dump.json>

Ausgabe:
    Pro Unterschied eine Zeile: PFAD  |  dm=WERT  |  swx=WERT
    Zusammenfassung am Ende.

Exit-Code:
    0  — keine Unterschiede
    1  — Unterschiede gefunden
    2  — Fehler beim Laden
"""

import datetime
import json
import re
import sys
import argparse
from pathlib import Path, PureWindowsPath


# ── Normalisierung ────────────────────────────────────────────────────────────

# Properties, die von libopenswx bewusst nicht gelesen werden oder DM-intern sind.
# Unterschiede in diesen Feldern werden stillschweigend ignoriert.
_SKIP_PREFIXES = ("PX:", "PHOENIX:", "PWDB_", "MaxxDB", "WMSO:")

# DM-interne Properties, die nicht im XML stehen (nie in swx vorhanden).
_SKIP_EXACT = {"__IsSubFolder", "___ParentConfigName"}


def _is_skip_key(key: str) -> bool:
    return key in _SKIP_EXACT or any(key.startswith(p) for p in _SKIP_PREFIXES)


_GERMAN_MONTHS = {
    "Januar": 1, "Februar": 2, "März": 3, "April": 4,
    "Mai": 5, "Juni": 6, "Juli": 7, "August": 8,
    "September": 9, "Oktober": 10, "November": 11, "Dezember": 12,
}
_GERMAN_DATE_RE = re.compile(
    r"\w+,\s+(\d+)\.\s+(\w+)\s+(\d{4})"
)


def _parse_date_ymd(s: str):
    """Versucht, ein Datum aus einem String zu lesen. Gibt (year, month, day) zurück."""
    if not s:
        return None
    # ISO 8601: 2021-07-20T12:06:42Z
    m = re.match(r"(\d{4})-(\d{2})-(\d{2})", s)
    if m:
        return (int(m.group(1)), int(m.group(2)), int(m.group(3)))
    # Deutsches Langdatum: "Dienstag, 20. Juli 2021 14:06:42"
    m = _GERMAN_DATE_RE.match(s)
    if m:
        month = _GERMAN_MONTHS.get(m.group(2))
        if month:
            return (int(m.group(3)), month, int(m.group(1)))
    # Deutsches Kurzdatum: "20.07.2021" oder "20.7.2021"
    m = re.match(r"(\d{1,2})\.(\d{1,2})\.(\d{4})", s)
    if m:
        return (int(m.group(3)), int(m.group(2)), int(m.group(1)))
    return None


# Felder, bei denen nur das Datum (ohne Uhrzeit/Zeitzone) verglichen wird.
# Die DM gibt diese Werte in lokaler Zeitzone aus; swx gibt UTC-ISO-8601 zurück.
# Ein Unterschied von ±1 Tag wird toleriert (UTC-Mitternacht-Grenzfall).
_DATE_KEYS = {
    "CreateDateTime", "LastSaveDateTime",
    # Benutzerdefinierte Datumsfelder (vt:filetime in custom.xml)
    "DesignedDate", "Erstellt am", "Gez. am", "Geprüft am",
    "_for_80590_approval_date", "_for_80611_approval_date",
    "_for_80642_approval_date", "classification_date_for_80642",
    "creation_date_for_80611",
}


_SW_EMPTY_VALUES = {"[R] -", "[R]-"}  # SolidWorks formula placeholder for empty/unresolved


def norm_str(v: str) -> str:
    """Whitespace bereinigen. SolidWorks-Leerformel auf '' normieren.
    CRLF → LF: Der DM liefert LPSTR-Werte mit Windows-Zeilenenden."""
    if not isinstance(v, str):
        return v
    s = v.replace("\r\n", "\n").replace("\r", "\n").strip()
    return "" if s in _SW_EMPTY_VALUES else s


def norm_val(key: str, v) -> str:
    """Wert normalisieren: bei Datumsfeldern nur Jahr-Monat-Tag vergleichen."""
    s = norm_str(v) if isinstance(v, str) else str(v) if v is not None else ""
    if key in _DATE_KEYS:
        parsed = _parse_date_ymd(s)
        if parsed:
            return f"{parsed[0]:04d}-{parsed[1]:02d}-{parsed[2]:02d}"
    return s


def norm_path_basename(p: str) -> str:
    """Nur den Dateinamen (ohne Verzeichnis), Backslash→Slash, Kleinschreibung."""
    if not p:
        return ""
    # PureWindowsPath versteht auch Linux-Pfade mit Backslash
    return PureWindowsPath(p).name.lower()


def norm_props(raw: dict) -> dict:
    """Leere Werte entfernen, Whitespace streifen."""
    return {k: norm_str(v) for k, v in raw.items() if norm_str(v or "")}


# ── Differenz-Sammler ─────────────────────────────────────────────────────────

class DiffCollector:
    def __init__(self):
        self.diffs: list[tuple[str, object, object]] = []

    def add(self, path: str, dm_val, swx_val):
        self.diffs.append((path, dm_val, swx_val))

    def ok(self) -> bool:
        return len(self.diffs) == 0


# ── Feldvergleiche ────────────────────────────────────────────────────────────

def cmp_props(dm: dict, swx: dict, ctx: str, col: DiffCollector,
              strict: bool = False):
    """Vergleicht Properties.

    Im Standardmodus (strict=False) werden Felder ignoriert, die dm nicht hat
    aber swx hat (swx liest manchmal mehr aus dem XML als die DM-API liefert).
    Mit strict=True werden solche Felder ebenfalls als Unterschied gewertet.
    """
    all_keys = set(dm) | set(swx)
    for key in sorted(all_keys):
        if _is_skip_key(key):
            continue
        d = dm.get(key, "")
        s = swx.get(key, "")
        # Im Standardmodus: swx hat Wert, dm hat keinen → kein Fehler.
        if not strict and not norm_val(key, d) and norm_val(key, s):
            continue
        if norm_val(key, d) != norm_val(key, s):
            # Datumsfelder: ±1 Tag tolerieren (DM gibt Lokalzeit, swx UTC).
            # Ein Tag Unterschied entsteht, wenn das Dokument kurz vor/nach
            # UTC-Mitternacht gespeichert wurde (z.B. 23:00 UTC = 01:00 MEZ).
            if key in _DATE_KEYS:
                d_ymd = _parse_date_ymd(norm_str(d))
                s_ymd = _parse_date_ymd(norm_str(s))
                if d_ymd and s_ymd:
                    delta = abs(
                        (datetime.date(*d_ymd) - datetime.date(*s_ymd)).days
                    )
                    if delta <= 1:
                        continue
            col.add(f"{ctx}.{key}", d, s)


def cmp_component(dm_c: dict, swx_c: dict, ctx: str, col: DiffCollector):
    # Pfad: nur Basename vergleichen (Windows-Pfad vs. Windows-Pfad aus Datei)
    d_path = norm_path_basename(dm_c.get("path", ""))
    s_path = norm_path_basename(swx_c.get("path", ""))
    if d_path != s_path:
        col.add(f"{ctx}.path", dm_c.get("path", ""), swx_c.get("path", ""))

    # Konfigurationsname
    d_cfg = norm_str(dm_c.get("ref_config", ""))
    s_cfg = norm_str(swx_c.get("ref_config", ""))
    if d_cfg != s_cfg:
        col.add(f"{ctx}.ref_config", d_cfg, s_cfg)

    # Boolesche Felder
    for field in ("is_suppressed", "is_hidden", "exclude_from_bom"):
        if dm_c.get(field) != swx_c.get(field):
            col.add(f"{ctx}.{field}", dm_c.get(field), swx_c.get(field))

    # component_reference: DM computes instance IDs (e.g. "Part-1"),
    # XML often stores empty string.  Skip if swx has no value but dm does.
    d_ref = norm_str(dm_c.get("component_reference", ""))
    s_ref = norm_str(swx_c.get("component_reference", ""))
    if d_ref != s_ref and not (d_ref and not s_ref):
        col.add(f"{ctx}.component_reference", d_ref, s_ref)


def _comp_key(c: dict) -> str:
    """Eindeutiger Schlüssel für Komponentenabgleich: Basename + Name."""
    return norm_path_basename(c.get("path", "")) + "/" + norm_str(c.get("name", ""))


def cmp_components(dm_list: list, swx_list: list, ctx: str, col: DiffCollector):
    dm_by = {_comp_key(c): c for c in dm_list}
    swx_by = {_comp_key(c): c for c in swx_list}

    for key in sorted(set(dm_by) | set(swx_by)):
        cctx = f"{ctx}[{key!r}]"
        if key not in dm_by:
            col.add(cctx, "(fehlt in dm)", repr(swx_by[key].get("name")))
        elif key not in swx_by:
            col.add(cctx, repr(dm_by[key].get("name")), "(fehlt in swx)")
        else:
            cmp_component(dm_by[key], swx_by[key], cctx, col)


def cmp_cut_list_item(dm_i: dict, swx_i: dict, ctx: str, col: DiffCollector):
    if dm_i.get("quantity") != swx_i.get("quantity"):
        col.add(f"{ctx}.quantity", dm_i.get("quantity"), swx_i.get("quantity"))
    cmp_props(
        norm_props(dm_i.get("properties", {})),
        norm_props(swx_i.get("properties", {})),
        f"{ctx}.properties", col,
    )


def cmp_cut_list(dm_list: list, swx_list: list, ctx: str, col: DiffCollector):
    dm_by  = {norm_str(i.get("name", "")): i for i in dm_list}
    swx_by = {norm_str(i.get("name", "")): i for i in swx_list}
    for name in sorted(set(dm_by) | set(swx_by)):
        ictx = f"{ctx}[{name!r}]"
        if name not in dm_by:
            col.add(ictx, "(fehlt in dm)", "(vorhanden)")
        elif name not in swx_by:
            col.add(ictx, "(vorhanden)", "(fehlt in swx)")
        else:
            cmp_cut_list_item(dm_by[name], swx_by[name], ictx, col)


def cmp_config(dm_cfg: dict, swx_cfg: dict, ctx: str, col: DiffCollector):
    # Properties
    cmp_props(
        norm_props(dm_cfg.get("properties", {})),
        norm_props(swx_cfg.get("properties", {})),
        f"{ctx}.properties", col,
    )
    # Komponenten
    cmp_components(
        dm_cfg.get("components", []),
        swx_cfg.get("components", []),
        f"{ctx}.components", col,
    )
    # Cut-List-Einträge
    cmp_cut_list(
        dm_cfg.get("cut_list", []),
        swx_cfg.get("cut_list", []),
        f"{ctx}.cut_list", col,
    )


def cmp_configurations(dm_list: list, swx_list: list, ctx: str, col: DiffCollector):
    dm_by = {c["name"]: c for c in dm_list}
    swx_by = {c["name"]: c for c in swx_list}

    for name in sorted(set(dm_by) | set(swx_by)):
        cctx = f"{ctx}[{name!r}]"
        if name not in dm_by:
            # swx has a config dm doesn't: do not report in standard mode.
            # Unnamed configs ('') are internal/derived configs DM doesn't expose.
            # Named extra configs (e.g. flat-pattern 'StandardSM-FLAT-PATTERN')
            # represent extra data that swx reads beyond what DM returns.
            continue
        elif name not in swx_by:
            col.add(cctx, "(vorhanden)", "(fehlt in swx)")
        else:
            cmp_config(dm_by[name], swx_by[name], cctx, col)


def cmp_view(dm_v: dict, swx_v: dict, ctx: str, col: DiffCollector):
    d_doc = norm_path_basename(dm_v.get("referenced_document", ""))
    s_doc = norm_path_basename(swx_v.get("referenced_document", ""))
    if d_doc != s_doc:
        col.add(f"{ctx}.referenced_document",
                dm_v.get("referenced_document"), swx_v.get("referenced_document"))

    d_cfg = norm_str(dm_v.get("referenced_config", ""))
    s_cfg = norm_str(swx_v.get("referenced_config", ""))
    if d_cfg != s_cfg:
        col.add(f"{ctx}.referenced_config", d_cfg, s_cfg)


def cmp_sheets(dm_list: list, swx_list: list, ctx: str, col: DiffCollector):
    dm_by = {s["name"]: s for s in dm_list}
    swx_by = {s["name"]: s for s in swx_list}

    for name in sorted(set(dm_by) | set(swx_by)):
        sctx = f"{ctx}[{name!r}]"
        if name not in dm_by:
            col.add(sctx, "(fehlt in dm)", "(vorhanden)")
            continue
        if name not in swx_by:
            col.add(sctx, "(vorhanden)", "(fehlt in swx)")
            continue

        dm_views = {v["name"]: v for v in dm_by[name].get("views", [])}
        swx_views = {v["name"]: v for v in swx_by[name].get("views", [])}
        for vname in sorted(set(dm_views) | set(swx_views)):
            vctx = f"{sctx}.views[{vname!r}]"
            if vname not in dm_views:
                col.add(vctx, "(fehlt in dm)", "(vorhanden)")
            elif vname not in swx_views:
                col.add(vctx, "(vorhanden)", "(fehlt in swx)")
            else:
                cmp_view(dm_views[vname], swx_views[vname], vctx, col)


def compare_documents(dm: dict, swx: dict) -> DiffCollector:
    col = DiffCollector()

    # doc_type
    if dm.get("doc_type") != swx.get("doc_type"):
        col.add("doc_type", dm.get("doc_type"), swx.get("doc_type"))

    # version
    if dm.get("version") != swx.get("version"):
        col.add("version", dm.get("version"), swx.get("version"))

    # global_properties
    cmp_props(
        norm_props(dm.get("global_properties", {})),
        norm_props(swx.get("global_properties", {})),
        "global_properties", col,
    )

    # configurations
    cmp_configurations(
        dm.get("configurations", []),
        swx.get("configurations", []),
        "configurations", col,
    )

    # sheets (nur für Zeichnungen relevant)
    cmp_sheets(dm.get("sheets", []), swx.get("sheets", []), "sheets", col)

    return col


# ── Ausgabe ───────────────────────────────────────────────────────────────────

def print_report(label: str, col: DiffCollector, verbose: bool = True) -> None:
    if col.ok():
        print(f"  OK  {label}")
        return

    print(f"  DIFF {label}  ({len(col.diffs)} Unterschied(e))")
    if verbose:
        for path, dm_val, swx_val in col.diffs:
            print(f"       {path}")
            print(f"         dm : {dm_val!r}")
            print(f"         swx: {swx_val!r}")


# ── Einstiegspunkt ────────────────────────────────────────────────────────────

def load_json(path: str) -> dict:
    try:
        with open(path, encoding="utf-8-sig") as fh:
            return json.load(fh)
    except Exception as exc:
        print(f"Fehler beim Laden von {path!r}: {exc}", file=sys.stderr)
        sys.exit(2)


def main():
    parser = argparse.ArgumentParser(
        description="Vergleicht swdm_dump- und swx_dump-JSON-Dateien."
    )
    parser.add_argument("dm_json",  help="JSON von swdm_dump  (Document Manager)")
    parser.add_argument("swx_json", help="JSON von swx_dump   (libopenswx)")
    parser.add_argument("-q", "--quiet", action="store_true",
                        help="Nur Zusammenfassung, keine Detaildiffs")
    args = parser.parse_args()

    dm  = load_json(args.dm_json)
    swx = load_json(args.swx_json)

    col = compare_documents(dm, swx)
    label = f"{Path(args.dm_json).stem}"
    print_report(label, col, verbose=not args.quiet)

    sys.exit(0 if col.ok() else 1)


if __name__ == "__main__":
    main()
