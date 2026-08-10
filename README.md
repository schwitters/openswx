# openswx

A cross-platform toolkit for reading SolidWorks® files (`.SLDPRT`, `.SLDASM`, `.SLDDRW`) without a SolidWorks installation, COM, or any Windows-only dependency.

---

## Overview

The repository contains three independent components:

```
openswx/
├── libopenswx/   Pure C++20 library — parses SolidWorks files
├── libopenbom/   Pure C++20 library — builds hierarchical BOMs (built on libopenswx)
└── asmbox/       HTTP server + CLI tool — built on top of both libraries
```

### libopenswx

A zero-dependency C++20 static library that reads both SolidWorks file formats:

| Format | Versions | Detection |
|--------|----------|-----------|
| Modern chunk format | SW 2015 and later | byte scan for `14 00 06 00 08 00` marker |
| OLE2 Compound Document | SW 2014 and earlier | magic `D0 CF 11 E0 A1 B1 1A E1` |

Extracted data per document:

- **Document type** (part / assembly / drawing)
- **Model version** number
- **Global custom properties** (`docProps/custom.xml`)
- **Document preview** PNG thumbnail
- Per **configuration**: name, custom properties, mass properties (CoG, volume, surface area, mass, inertia tensor), preview PNG, cut-list items (weldments), assembly component references
- Per **drawing sheet**: name, preview PNG, view references

### libopenbom

A C++20 static library that builds hierarchical, recursive Bills of Materials from SolidWorks files. It resolves Windows component paths to host-filesystem paths, aggregates identical instances, and serialises the result to JSON.

Key types:

| Type | Description |
|------|-------------|
| `BomTransformer` | Entry point — call `Build(path)` to produce a `Bom` |
| `BomTransformerConfig` | Controls configuration selection, filtering, recursion depth, path resolution |
| `PathResolverConfig` | Maps Windows paths to host paths via prefix substitutions and directory search |
| `BomItem` | One row in the BOM tree (name, quantity, properties, children) |
| `Bom` | Root `BomItem` plus accumulated warnings |
| `JsonWriter` | Serialises a `Bom` to JSON string or file |

### asmbox

A Linux HTTP server that lets you upload a ZIP archive of SolidWorks files and browse the extracted metadata, BOM trees, and custom properties in a web UI.  It also ships `swx_dump`, a command-line tool that prints a JSON representation of any single SolidWorks file.

---

## Prerequisites

| Package | Minimum version | Purpose |
|---------|----------------|---------|
| CMake | 3.20 | Build system |
| GCC or Clang | C++20 capable (GCC 12+) | Compiler |
| zlib (`libz-dev`) | any | Raw-deflate decompression in libopenswx |
| pugixml (`libpugixml-dev`) | 1.11+ | XML parsing in libopenswx |
| nlohmann/json (`nlohmann-json3-dev`) | 3.x | JSON serialization in asmbox |
| OpenSSL (`libssl-dev`) | 3.x | Password hashing and session token support in asmbox |
| SQLite3 (`libsqlite3-dev`) | 3.x | Result caching and BOM persistence in asmbox |
| libzip (`libzip-dev`) | 1.x | ZIP upload handling in asmbox |

The following are fetched automatically by CMake's `FetchContent` during configuration:

- **Crow** v1.2.0 — HTTP server framework
- **plog** v1.1.10 — Logging
- **googletest** v1.15.2 — Unit tests

On Debian/Ubuntu, install the system packages with:

```bash
sudo apt install cmake build-essential \
    libz-dev libssl-dev libpugixml-dev nlohmann-json3-dev \
    libsqlite3-dev libzip-dev
```

---

## Building

```bash
cd openswx

# Configure (downloads Crow, plog, googletest automatically)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Build everything
cmake --build build -j$(nproc)
```

This produces three binaries inside `build/asmbox/`:

| Binary | Description |
|--------|-------------|
| `asmbox` | HTTP server (default port 8087) |
| `swx_dump` | CLI tool — prints JSON to stdout |
| `swx_scan` | CLI tool — scans directories and writes anonymized aggregate reports |
| `asmbox_tests` | Unit test suite |

Run the tests:

```bash
./build/asmbox/asmbox_tests
```

---

## Tutorial

### 1. Inspect a single file with `swx_dump`

```bash
./build/asmbox/swx_dump /path/to/part.SLDPRT | python3 -m json.tool
```

Example output (truncated):

```json
{
  "doc_type": 1,
  "version": 17000,
  "global_properties": {
    "Desc1": "Winkelstecker",
    "Material": "Messing"
  },
  "configurations": [
    {
      "name": "Standard",
      "properties": { "Configuration": "Standard" },
      "mass_properties": {
        "mass": 0.0251,
        "volume": 2.5e-06,
        "surface_area": 0.00654,
        "center_of_gravity": { "x": -0.003, "y": 0.0, "z": 0.013 }
      }
    }
  ]
}
```

`doc_type` values: `1` = part, `2` = assembly, `3` = drawing.
All mass/geometry values are in SI units (kg, m, m², m³).

---

### 2. Run the asmbox web server

```bash
./build/asmbox/asmbox
```

The server starts at `http://0.0.0.0:8087`.

Open `http://localhost:8087` in a browser to reach the upload UI.

On first use, create a local user account in the sign-in dialog. Workspaces and
profiles are private to the signed-in user and are no longer shared globally
between browser sessions.

To bootstrap an admin account on server start, set both
`ASMBOX_ADMIN_USER` and `ASMBOX_ADMIN_PASSWORD`. If the named user already
exists, `asmbox` promotes that user to admin and keeps the existing password.

```bash
ASMBOX_ADMIN_USER=admin \
ASMBOX_ADMIN_PASSWORD='change-me-now' \
  ./build/asmbox/asmbox
```

You can also create or promote an admin explicitly via CLI:

```bash
./build/asmbox/asmbox create-admin admin --password 'change-me-now'
```

If `--password` is omitted, `asmbox` reads the password from
`ASMBOX_ADMIN_PASSWORD` or prompts on stdin.

**Upload a ZIP archive** containing one or more SolidWorks files. The server will:
1. Unpack the archive into `ASMBOX_DATA_DIR/<workspace>/`
2. Scan for `.SLDPRT`, `.SLDASM`, and `.SLDDRW` files
3. Analyze each file immediately and cache the result in SQLite
4. Return the list of found files as JSON

**Browse the workspace** in the sidebar, click any file to see its parsed document data and properties.

**View the BOM** via the "Bill of Materials" tab for assembly and drawing files.  The BOM is built recursively by resolving component paths within the workspace directory.

**Configure properties** per workspace: mark which properties are visible, and assign semantic roles (`part_number`, `description`, `material`, `revision`).  The property designated as `part_number` is used when persisting BOMs.

**Profiles** let you define reusable mapping and transformation rules that are independent of any workspace (they survive workspace deletion and server restarts):
- **Mappings** link SolidWorks custom property names to structured fields in the output (Part, BomItem, or Bom Thrift entities).
- **Rules** transform the BOM tree on the fly:
  - `kaufgruppe` — keep the matched item but remove its children.
  - `non_bom` — exclude the matched item entirely.
  - `phantom` — exclude the matched item and promote its children one level up.

A profile is applied to a BOM request by appending `?profile=<id>` to the URL.

**Resizable panes** — drag the dividers in the Details view, Bill of Materials view, and sidebar to resize panes.  Widths are persisted in `localStorage`.

---

### 3. Scan a directory tree with `swx_scan`

```bash
./build/asmbox/swx_scan /path/to/archive
```

This recursively scans `.SLDPRT`, `.SLDASM`, and `.SLDDRW` files and prints
only aggregate compatibility statistics.  It does not emit filenames, paths,
 custom properties, configuration names, or other CAD metadata.

To write an anonymized JSON report:

```bash
./build/asmbox/swx_scan --report report.json /path/to/archive
```

The JSON report contains only aggregate counters such as total files,
successful parses, failed parses, document types, detected versions, and
stable error categories.

---

### 4. Container image

Build the image locally:

```bash
docker build -t openswx:latest .
```

Run `asmbox` with persistent workspace data:

```bash
docker run --rm \
  -p 8087:8087 \
  -v openswx-data:/data \
  openswx:latest
```

Run `swx_dump` directly through `--entrypoint`:

```bash
docker run --rm \
  --entrypoint swx_dump \
  -v "$PWD:/files:ro" \
  openswx:latest \
  /files/example.SLDPRT
```

The same image also works with `podman`, for example:

```bash
podman build -t openswx:latest .
podman run --rm -p 8087:8087 -v openswx-data:/data openswx:latest
```

The image runs as a non-root user, exposes port `8087`, stores persistent
state in `/data`, and includes `asmbox`, `swx_dump`, and `swx_scan`.

---

### 5. Configuration via environment variables

All settings use the `ASMBOX_` prefix and have sensible defaults:

| Variable | Default | Description |
|----------|---------|-------------|
| `ASMBOX_BIND_ADDR` | `0.0.0.0` | IP address to bind to |
| `ASMBOX_PORT` | `8087` | TCP port |
| `ASMBOX_DATA_DIR` | `$TMPDIR/sw_portal` | Root directory for workspaces and the SQLite database |
| `ASMBOX_TEMPLATE_DIR` | `<binary-dir>/templates` | Directory containing the Crow HTML templates |
| `ASMBOX_ADMIN_USER` | unset | Optional bootstrap admin username |
| `ASMBOX_ADMIN_PASSWORD` | unset | Optional bootstrap admin password |

The main SQLite database (`main.sqlite`) is stored in `ASMBOX_DATA_DIR` and is
persistent across server restarts. Workspace-specific caches are stored in each
workspace directory as `workspace.sqlite`. The schema is created with
`CREATE TABLE IF NOT EXISTS`, so restarting the server never erases cached data
or profiles.

Example — run on a non-default port, binding only to localhost, with a persistent data directory:

```bash
ASMBOX_PORT=9090 \
ASMBOX_BIND_ADDR=127.0.0.1 \
ASMBOX_DATA_DIR=/var/lib/asmbox \
ASMBOX_TEMPLATE_DIR=/usr/share/asmbox/templates \
  ./build/asmbox/asmbox
```

---

### 6. Using libopenswx in your own project

Add libopenswx as a subdirectory and link against it:

```cmake
add_subdirectory(path/to/openswx/libopenswx)
target_link_libraries(my_target PRIVATE openswx)
```

Minimal usage:

```cpp
#include "openswx/document.h"

auto result = openswx::SwxDocument::Open("part.SLDPRT");
if (!result.ok()) {
    std::cerr << result.error() << "\n";
    return 1;
}

const openswx::Document& doc = result.value().doc();

for (const auto& cfg : doc.configurations) {
    std::cout << "Config: " << cfg.name << "\n";
    if (cfg.mass_properties) {
        std::cout << "  Mass: " << cfg.mass_properties->mass << " kg\n";
    }
    for (const auto& [key, val] : cfg.properties) {
        std::cout << "  " << key << " = " << val << "\n";
    }
}
```

---

### 7. Using libopenbom in your own project

Add both libraries and link:

```cmake
add_subdirectory(path/to/openswx/libopenswx)
add_subdirectory(path/to/openswx/libopenbom)
target_link_libraries(my_target PRIVATE openbom openswx)
```

Minimal usage:

```cpp
#include "openbom/transformer.h"
#include "openbom/json_writer.h"

openbom::BomTransformerConfig cfg;

// Map Windows component paths to the host filesystem.
cfg.path_resolver.substitutions.push_back(
    {"C:\\Engineering\\Parts\\", "/mnt/nas/parts/"});

// Or simply search a local directory by filename.
cfg.path_resolver.search_dirs.push_back("/data/parts");
cfg.path_resolver.recursive_search = true;
cfg.path_resolver.case_insensitive = true;

openbom::BomTransformer transformer(cfg);
auto result = transformer.Build("/data/top_asm.SLDASM");
if (!result.ok()) {
    std::cerr << result.error() << "\n";
    return 1;
}

const openbom::Bom& bom = result.value();
for (const auto& w : bom.warnings)
    std::cerr << "Warning: " << w << "\n";

openbom::JsonWriter writer;
writer.WriteToFile(bom, "bom.json");
```

`BomTransformerConfig` options:

| Field | Default | Description |
|-------|---------|-------------|
| `configuration_name` | `""` | SolidWorks configuration to activate; empty = first |
| `include_suppressed` | `false` | Include suppressed assembly components |
| `include_hidden` | `false` | Include hidden assembly components |
| `include_exclude_from_bom` | `false` | Include components flagged "exclude from BOM" |
| `expand_cut_lists` | `true` | Add weldment cut-list items as children |
| `max_depth` | `0` | Max recursion depth (0 = unlimited) |
| `path_resolver` | — | `PathResolverConfig` for Windows-to-host path mapping |
| `item_callback` | `nullptr` | Optional post-processing callback per `BomItem` |

---

## REST API reference

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/` | Web UI (`index.html`) |
| `GET` | `/api/workspaces` | List all workspaces as JSON |
| `DELETE` | `/api/workspaces/<name>` | Delete a workspace and its files |
| `POST` | `/upload` | Upload a `multipart/form-data` request with a `file` field (ZIP) |
| `GET` | `/api/doc/<workspace>/<path_b64>` | Parsed document JSON for one file (cached in SQLite) |
| `GET` | `/api/workspaces/<ws>/props` | Property names + configuration for the workspace |
| `POST` | `/api/workspaces/<ws>/props` | Save property configuration (visibility, roles) |
| `GET` | `/api/profiles` | List all profiles |
| `POST` | `/api/profiles` | Create a profile `{name, description}` |
| `GET` | `/api/profiles/<id>` | Full profile (name, description, mappings, rules) |
| `PUT` | `/api/profiles/<id>` | Save full profile (upsert mappings and rules) |
| `DELETE` | `/api/profiles/<id>` | Delete a profile |
| `GET` | `/api/bom/<workspace>/<path_b64>` | Build BOM; optionally apply profile with `?profile=<id>` |

`<path_b64>` is the relative file path within the workspace, Base64url-encoded.

---

## Architecture notes

```
SolidWorks file
      │
      ▼
libopenswx::ParseFile()
  ├─ IsOle2() → ParseOle2Format()   (OLE2 FAT/directory/sector chain)
  └─ ParseModernFormat()            (marker scan + ROL-decoded names + raw deflate)
      │
      ▼
   StreamMap  (flat map: stream-path → decompressed bytes)
      │
      ├─ ParsePropertyXml()         docProps/custom.xml, Config-N-Properties.xml
      ├─ ParseMassProperties()      docProps/ISolidWorksInformation.xml
      ├─ ParseCutlistXml()          docProps/Config-N-Cutlist-Properties.xml
      ├─ ParseComponents()          swXmlContents/COMPINSTANCETREE
      ├─ ParseSheetNames()          SheetPreviews/SheetNames
      └─ ExtractPng()               PreviewPNG / Config-N-PreviewPNG

libopenbom::BomTransformer::Build()
      │
      ├─ Detect file type (part / assembly / drawing)
      ├─ Select active configuration
      ├─ Recursively expand assembly components
      │    ├─ PathResolver: Windows path → host path
      │    │    ├─ Prefix substitution rules
      │    │    └─ Directory search (shallow + optional recursive)
      │    └─ Aggregate identical instances (quantity > 1)
      └─ Return Bom { root: BomItem, warnings: [...] }

asmbox (server.cc)
      │
      ├─ POST /upload        → unzip, scan, eager-analyze all SWX files
      ├─ GET  /api/doc       → load or analyze + cache in SQLite
      ├─ GET  /api/bom       → BomTransformer::Build() + ApplyBomRules(profile)
      │                        + SaveBom() to SQLite (surrogate-key schema)
      ├─ GET|POST /api/workspaces/<ws>/props  → property config CRUD
      └─ /api/profiles[/<id>]                → profile CRUD (workspace-independent)

SQLite schema (global.sqlite, persistent)
      ├─ workspaces, files, documents, properties
      ├─ boms, bom_items          (surrogate-key BOM persistence)
      ├─ property_configs         (per-workspace property visibility + roles)
      └─ profiles, profile_mappings, profile_rules  (workspace-independent)
```

Both file formats are detected automatically. The library never writes to disk and makes no system calls beyond reading the file.
