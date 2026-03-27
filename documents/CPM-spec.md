# CPM — Chaos Package Manager Specification
## AIOS v2 Application Distribution System (Phase 12)

---

## Preface: Why A Package Manager

AIOS has nine built-in apps, a working desktop, and a full networking stack. Apps are directory-based with `manifest.lua` metadata, `.cpk` archives provide compression and bundling, and `aios.net.*` gives Lua HTTP/HTTPS access via lwIP and BearSSL. The only thing missing is the glue: a system that connects a remote repository of packages to the local install/update/remove lifecycle.

CPM (Chaos Package Manager) is that glue. It is not a complex system. The server is static files on GitHub Pages. The client is a Lua app using APIs that already exist. The protocol is HTTP GET. The package format is `.cpk`, which is already implemented. There is no new C code in this spec — everything builds on Phase 7 (Lua), Phase 11 (networking), and the existing LZ4/CPK infrastructure.

**Design rules:**
1. **The server is static files.** No backend, no database, no API server. A GitHub repository with GitHub Pages enabled serves the repository index and `.cpk` files over HTTPS. Publishing a package means committing files and pushing.
2. **The repository index is a Lua table.** Not JSON, not XML, not a custom binary format. `load()` parses it directly. Consistent with the AIOS philosophy that Lua tables are the universal data structure.
3. **The local database is a Lua table.** Written to ChaosFS as a file. Same format, same tooling, same mental model.
4. **Every operation uses existing APIs.** `aios.net.http_get()` for downloads. `aios.cpk.*` for extraction. `aios.io.*` for filesystem operations. CRC-32 for integrity (same algorithm as ChaosFS superblock checksums). No new kernel-level functionality required.
5. **Path containment is mandatory.** A package can only install files under `/apps/<package-name>/`. A package cannot write to `/system/`, `/data/`, or any other app's directory. This is enforced by the installer, not the package format.
6. **Backward compatible.** Single-file `.lua` apps in `/apps/` keep working. The package manager and directory-based apps coexist with the existing app scanner. CPM manages what it installs; it does not claim ownership of manually placed apps.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                  GitHub Pages (HTTPS)                     │
│                                                          │
│  farmerbob1.github.io/chaos-repo/                        │
│    ├── repo.lua              ← repository index          │
│    └── packages/                                         │
│        ├── doom-1.0.cpk                                  │
│        ├── calculator-2.1.cpk                            │
│        └── paint-1.3.cpk                                 │
└──────────────────────┬──────────────────────────────────┘
                       │ HTTPS GET (BearSSL + lwIP)
                       │
┌──────────────────────▼──────────────────────────────────┐
│                  CPM Client (Lua)                         │
│                                                          │
│  /apps/cpm/main.lua          ← GUI app                   │
│  /system/lib/cpm.lua         ← core library               │
│                                                          │
│  Uses: aios.net.http_get()   ← download files            │
│        aios.cpk.*            ← extract archives           │
│        aios.io.*             ← filesystem operations      │
│        load()                ← parse Lua tables           │
└──────────────────────┬──────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────┐
│                  Local State (ChaosFS)                    │
│                                                          │
│  /system/cpm/                                            │
│    ├── installed.lua         ← local package database    │
│    ├── sources.lua           ← repository URL list       │
│    └── cache/                ← downloaded .cpk files     │
│        ├── doom-1.0.cpk                                  │
│        └── calculator-2.1.cpk                            │
│                                                          │
│  /apps/<name>/               ← installed app directories │
│    ├── manifest.lua                                      │
│    ├── main.lua                                          │
│    └── ...                                               │
└─────────────────────────────────────────────────────────┘
```

---

## Repository Format

### Repository Index (`repo.lua`)

The repository index is a Lua file that returns a table. The CPM client downloads this file and executes it with `load()` to get the package list. This is the single source of truth for what packages are available.

```lua
-- repo.lua — Chaos Package Repository Index
-- This file is served as a static file via GitHub Pages.
-- CPM downloads and load()'s it to get the package list.

return {
    -- Repository metadata
    repo_version = 1,           -- index format version (for future compat)
    repo_name = "AIOS Official",
    updated = "2026-03-23",     -- last time this index was regenerated

    -- Package list
    packages = {
        {
            name        = "doom",
            version     = "1.0.0",
            author      = "AIOS Community",
            description = "Classic Doom engine port for AIOS",
            icon        = "icons/doom_32.png",      -- relative to package root
            size        = 524288,                    -- .cpk file size in bytes
            checksum    = 0xA3F7C9B2,               -- CRC-32 of the .cpk file
            url         = "packages/doom-1.0.0.cpk", -- relative to repo base URL
            depends     = {},                        -- package name strings
            min_aios    = "2.0",                     -- minimum AIOS version
            category    = "games",                   -- for UI grouping
        },
        {
            name        = "calculator",
            version     = "2.1.0",
            author      = "AIOS",
            description = "Scientific calculator with graphing",
            icon        = "icons/calc_32.png",
            size        = 32768,
            checksum    = 0x1B4E5D88,
            url         = "packages/calculator-2.1.0.cpk",
            depends     = {},
            min_aios    = "2.0",
            category    = "utilities",
        },
        {
            name        = "paint",
            version     = "1.3.0",
            author      = "AIOS Community",
            description = "Pixel art editor with layers",
            icon        = "icons/paint_32.png",
            size        = 98304,
            checksum    = 0x7C2FA011,
            url         = "packages/paint-1.3.0.cpk",
            depends     = {},
            min_aios    = "2.0",
            category    = "creative",
        },
    },
}
```

**Field contracts:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `name` | string | yes | Unique package identifier. Lowercase, alphanumeric + hyphens. Max 32 chars. This becomes the directory name under `/apps/`. |
| `version` | string | yes | Semantic version string (MAJOR.MINOR.PATCH). Compared lexicographically by the client for update detection. |
| `author` | string | yes | Display name of the package author. |
| `description` | string | yes | One-line description shown in the package manager UI. Max 128 chars. |
| `icon` | string | no | Relative path to icon within the `.cpk`. If absent, the client uses the generic app icon. |
| `size` | integer | yes | Size of the `.cpk` file in bytes. Displayed to user before download and used for progress indication. |
| `checksum` | integer | yes | CRC-32 of the `.cpk` file (same polynomial as ChaosFS: 0xEDB88320, reflected). Verified after download. Mismatch = download rejected. |
| `url` | string | yes | URL path to the `.cpk` file, relative to the repository base URL. The client prepends the base URL from `sources.lua`. |
| `depends` | table | yes | Array of package name strings that must be installed first. Empty table if no dependencies. |
| `min_aios` | string | no | Minimum AIOS version required. If absent, no version check. If present and the local AIOS version is lower, the package is shown as incompatible. |
| `category` | string | no | UI grouping hint. Known categories: `"games"`, `"utilities"`, `"creative"`, `"system"`, `"network"`, `"development"`. Unknown categories are shown under "Other". If absent, defaults to "Other". |

**Security note on `load()`:** The repository index is executed as Lua code. This is safe in the AIOS context because (a) there is no process isolation — all Lua runs in kernel space anyway, and (b) the repo URL is configured by the user, not by the package. If this trust model changes in the future (e.g., untrusted third-party repos), the index format should be switched to a static data format parsed without execution.

### Repository Filesystem Layout

```
chaos-repo/                          ← GitHub repository root
├── repo.lua                         ← package index
├── packages/                        ← .cpk files
│   ├── doom-1.0.0.cpk
│   ├── doom-1.1.0.cpk              ← old versions can coexist
│   ├── calculator-2.1.0.cpk
│   └── paint-1.3.0.cpk
└── README.md                        ← for humans browsing the repo
```

**Versioned filenames are mandatory.** The `.cpk` filename includes the version to allow old versions to coexist during transitions. The `url` field in `repo.lua` always points to the current version. Old `.cpk` files can be removed after a reasonable period.

### Publishing a Package

Publishing is a manual process — commit and push to the GitHub repository:

1. **Build the `.cpk`:** Use `tools/cpk_pack.py` to create the archive from the app's directory.
2. **Compute checksum:** The build tool should output the CRC-32 of the generated `.cpk` file.
3. **Copy the `.cpk`** to `packages/` in the repo.
4. **Update `repo.lua`:** Add or update the package entry with the new version, size, checksum, and URL.
5. **Git commit and push.** GitHub Pages serves the updated files within minutes.

**Future automation:** A GitHub Actions workflow could automate steps 2-4: on push of a new `.cpk` to `packages/`, the workflow regenerates `repo.lua` by scanning all `.cpk` files, reading their manifests, computing checksums, and writing the index. This is a convenience, not a requirement — the manual process works.

### `tools/cpk_publish.py` — Build Tool Extension

A host-side Python script that automates the publish preparation:

```
Usage: python tools/cpk_publish.py <app_dir> <repo_dir>

Steps:
  1. Reads <app_dir>/manifest.lua for name, version, description
  2. Runs cpk_pack.py to create <name>-<version>.cpk
  3. Computes CRC-32 of the .cpk
  4. Copies .cpk to <repo_dir>/packages/
  5. Updates <repo_dir>/repo.lua with the new/updated entry
  6. Prints summary (ready to git add/commit/push)
```

This tool runs on the development host, not inside AIOS.

---

## Local State

### Source List (`/system/cpm/sources.lua`)

Defines which repositories the client checks. Supports multiple repos for future extensibility (e.g., official + community + private).

```lua
-- /system/cpm/sources.lua
return {
    {
        name = "AIOS Official",
        url  = "https://farmerbob1.github.io/chaos-repo",
        enabled = true,
    },
    -- Additional repos can be added here:
    -- {
    --     name = "Community Packages",
    --     url  = "https://example.github.io/aios-community-repo",
    --     enabled = true,
    -- },
}
```

**URL contract:** The base URL must NOT end with a trailing slash. The client appends `/repo.lua` and `/packages/...` to it.

**Default:** On first boot (or if the file doesn't exist), the CPM client creates this file with the single official repo entry. The Settings app (or the CPM app itself) can add/remove/enable/disable repos.

### Installed Package Database (`/system/cpm/installed.lua`)

Tracks every package installed via CPM. This is the uninstall manifest and the update-comparison baseline.

```lua
-- /system/cpm/installed.lua
-- Written by CPM after each install/uninstall. Do not edit manually.
return {
    doom = {
        version      = "1.0.0",
        installed_at = 847293,           -- aios.os.ticks() at install time
        source       = "AIOS Official",  -- which repo it came from
        checksum     = 0xA3F7C9B2,       -- CRC-32 of the installed .cpk
        files = {                        -- every file extracted (for uninstall)
            "/apps/doom/manifest.lua",
            "/apps/doom/main.lua",
            "/apps/doom/lib/engine.lua",
            "/apps/doom/lib/renderer.lua",
            "/apps/doom/data/E1M1.wad",
            "/apps/doom/icons/doom_32.png",
        },
    },
    calculator = {
        version      = "2.1.0",
        installed_at = 531002,
        source       = "AIOS Official",
        checksum     = 0x1B4E5D88,
        files = {
            "/apps/calculator/manifest.lua",
            "/apps/calculator/main.lua",
            "/apps/calculator/lib/math_ext.lua",
            "/apps/calculator/icons/calc_32.png",
        },
    },
}
```

**Write contract:** This file is rewritten atomically after every install or uninstall operation. The write sequence is: write to `/system/cpm/installed.lua.tmp`, then rename to `/system/cpm/installed.lua`. This prevents a crash mid-write from corrupting the database. (Note: ChaosFS rename is same-directory only, which is satisfied here.)

**The `files` array is authoritative for uninstall.** When removing a package, CPM deletes exactly these files and then removes the package's directory. If a file in this list doesn't exist (e.g., user manually deleted it), CPM skips it and continues — uninstall is best-effort for individual files.

### Download Cache (`/system/cpm/cache/`)

Downloaded `.cpk` files are stored here before extraction. This serves two purposes:

1. **Retry on failed extraction.** If extraction crashes or is interrupted, the `.cpk` is still on disk and can be retried without re-downloading.
2. **Reinstall without network.** If a package needs reinstalling, the cached `.cpk` is used if its checksum matches the requested version.

**Cache eviction:** The cache is not automatically cleaned. The CPM app provides a "Clear Cache" button in its settings that deletes all files in `/system/cpm/cache/`. Users can also manually delete files from this directory.

**Cache size:** With LZ4-compressed `.cpk` files, typical apps are 10-100KB. Even with 50 cached packages, this is under 5MB — negligible on a 512MB disk.

---

## Client Library (`/system/lib/cpm.lua`)

The core logic is a Lua module, separate from the GUI. This allows terminal-based package management and programmatic use by other apps.

### API

```lua
local cpm = require "/system/lib/cpm"

-- Refresh the package index from all enabled sources.
-- Downloads repo.lua from each source, merges results.
-- Returns: { packages = { ... }, errors = { ... } }
-- errors is an array of { source = "name", error = "message" } for sources that failed.
local index = cpm.refresh()

-- Get list of installed packages.
-- Returns: table from installed.lua (keyed by package name).
local installed = cpm.installed()

-- Check for available updates.
-- Compares installed versions against the latest refresh()'d index.
-- Returns: array of { name, current_version, available_version, size }
local updates = cpm.check_updates()

-- Install a package by name.
-- Downloads .cpk, verifies checksum, extracts to /apps/<name>/, updates database.
-- Resolves and installs dependencies first (recursive, with cycle detection).
-- Returns: true on success, nil + error string on failure.
-- progress_cb: optional function(stage, detail) called during install.
--   stage: "resolving", "downloading", "verifying", "extracting", "finalizing"
--   detail: stage-specific info (e.g., bytes downloaded, file being extracted)
local ok, err = cpm.install("doom", progress_cb)

-- Uninstall a package by name.
-- Deletes all files from the installed database's file list.
-- Removes the /apps/<name>/ directory.
-- Updates the database.
-- Refuses if other installed packages depend on this one.
-- Returns: true on success, nil + error string on failure.
local ok, err = cpm.uninstall("doom")

-- Update a single package to the latest version.
-- Equivalent to uninstall + install, but preserves /apps/<name>/data/ if it exists.
-- Returns: true on success, nil + error string on failure.
local ok, err = cpm.update("doom", progress_cb)

-- Update all packages that have available updates.
-- Returns: { updated = { "doom", "paint" }, failed = { { name = "calc", error = "..." } } }
local results = cpm.update_all(progress_cb)

-- Search packages by name or description substring.
-- Searches the last refresh()'d index.
-- Returns: array of package entries matching the query.
local results = cpm.search("doom")

-- Get detailed info about a package (installed or available).
-- Returns: merged table with both repo info and local install info, or nil.
local info = cpm.info("doom")
```

### Install Sequence (Detail)

The `cpm.install(name, progress_cb)` function performs these steps in order. Any failure at any step aborts the install and cleans up partial state.

```
Step 1: RESOLVE
  - Look up package in the last refresh()'d index. Fail if not found.
  - Check min_aios against local AIOS version. Fail if incompatible.
  - Check if already installed at the same version. Return early if so.
  - Resolve dependencies recursively (depth-first).
    - Build a dependency list. Detect cycles (A depends on B depends on A).
    - For each uninstalled dependency, add it to the install queue.
  - progress_cb("resolving", { name = name, deps = dep_list })

Step 2: DOWNLOAD (for each package in install queue, including deps)
  - Check cache: if /system/cpm/cache/<name>-<version>.cpk exists
    and its CRC-32 matches the repo checksum, skip download.
  - Construct full URL: source.url .. "/" .. package.url
  - HTTP GET the .cpk file via aios.net.http_get() or the http.lua library.
  - Write response body to /system/cpm/cache/<name>-<version>.cpk
  - progress_cb("downloading", { name = name, bytes = size })

Step 3: VERIFY
  - Compute CRC-32 of the downloaded .cpk file.
  - Compare against repo checksum. Mismatch = delete cached file, fail.
  - progress_cb("verifying", { name = name, checksum = "ok" })

Step 4: EXTRACT
  - Open the .cpk via aios.cpk.open().
  - Enumerate all entries. For each entry:
    - Compute target path: "/apps/" .. name .. "/" .. entry.path
    - SECURITY CHECK: verify the resolved target path starts with "/apps/" .. name .. "/".
      Reject entries with ".." components, absolute paths, or paths that escape
      the package directory. Fail the entire install if any entry violates this.
    - Create parent directories as needed (aios.io.mkdir, recursive).
    - Extract file data (with LZ4 decompression if compressed).
    - Write to target path.
    - Record target path in the file list for the installed database.
  - progress_cb("extracting", { name = name, file = current_file })

Step 5: VALIDATE
  - Verify manifest.lua exists at /apps/<name>/manifest.lua.
  - Verify entry script exists (from manifest.entry field).
  - If validation fails, delete the extracted directory and fail.

Step 6: REGISTER
  - Add entry to installed.lua with version, timestamp, source, checksum, file list.
  - Write installed.lua atomically (write .tmp, rename).
  - Delete cached .cpk (optional — keep if cache policy says so).
  - progress_cb("finalizing", { name = name })
```

### Uninstall Sequence (Detail)

```
Step 1: CHECK DEPENDENTS
  - Scan installed.lua for any package whose depends list includes this package.
  - If any dependents exist, fail with error listing them.
  - User must uninstall dependents first, or use a force flag.

Step 2: DELETE FILES
  - Read the files array from installed.lua for this package.
  - Delete each file. Skip files that don't exist (no error).
  - Delete subdirectories bottom-up (deepest first), but only if empty.
  - Delete /apps/<name>/ if empty.

Step 3: UNREGISTER
  - Remove the package entry from installed.lua.
  - Write installed.lua atomically.
```

### Update Sequence (Detail)

```
Step 1: PRESERVE USER DATA
  - If /apps/<name>/data/ exists, rename to /apps/<name>_data_backup/
    (ChaosFS rename is same-directory — this needs to be a copy + delete,
     or the data directory needs to be moved to a temp location outside /apps/<name>/).
  - The data/ directory convention: apps that store user-created content
    (save files, settings, documents) should use /apps/<name>/data/.
    CPM preserves this directory across updates. Everything else is replaced.

Step 2: UNINSTALL (using uninstall sequence, but skip dependent check
         since we're reinstalling)

Step 3: INSTALL (using install sequence)

Step 4: RESTORE USER DATA
  - If backup exists, copy contents back to /apps/<name>/data/.
  - Delete backup.
```

### Error Handling

All CPM operations return `true` on success or `nil, error_string` on failure. Error strings are human-readable and include the operation context:

```lua
nil, "download failed: HTTP 404 for packages/doom-1.0.0.cpk"
nil, "checksum mismatch: expected 0xA3F7C9B2, got 0x00112233"
nil, "path escape rejected: entry '../../../system/wm.lua' in doom-1.0.0.cpk"
nil, "dependency cycle: doom -> engine -> doom"
nil, "cannot uninstall: calculator depends on mathlib"
nil, "incompatible: doom requires AIOS 3.0, running 2.0"
nil, "disk full: need 524288 bytes, only 102400 free"
```

The GUI app displays these to the user. The terminal can print them directly.

---

## Version Comparison

Package versions follow a simplified semantic versioning scheme: `MAJOR.MINOR.PATCH`, all integers. Comparison is numeric, left to right:

```lua
-- Returns -1 (a < b), 0 (a == b), or 1 (a > b)
function cpm._compare_versions(a, b)
    local a_maj, a_min, a_pat = a:match("^(%d+)%.(%d+)%.(%d+)$")
    local b_maj, b_min, b_pat = b:match("^(%d+)%.(%d+)%.(%d+)$")
    -- Convert to numbers and compare left to right
    -- If either version doesn't match the pattern, treat as string comparison fallback
end
```

**An update is available when** `compare_versions(installed_version, repo_version) < 0`.

---

## GUI Application (`/apps/cpm/`)

The CPM app is a standard AIOS directory-based app with `manifest.lua`.

### Manifest

```lua
-- /apps/cpm/manifest.lua
return {
    name = "Package Manager",
    version = "1.0.0",
    author = "AIOS",
    icon = "/system/icons/cpm_32.png",
    entry = "main.lua",
    description = "Install, update, and remove AIOS packages",
}
```

### UI Layout

The app uses a single window with a TabView containing three tabs:

**Tab 1: Browse**
- Displays all packages from the last `refresh()`, grouped by category.
- Each package shows: icon (if available), name, description, version, size.
- Status badge per package: "Installed", "Update Available", or "Install" button.
- Search bar at the top filters the list by name/description.
- "Refresh" button in the toolbar re-downloads `repo.lua` from all sources.

**Tab 2: Installed**
- Lists all locally installed packages from `installed.lua`.
- Each entry shows: icon, name, installed version, install date.
- Buttons per entry: "Update" (if available), "Uninstall".
- "Update All" button at the top when any updates are available.

**Tab 3: Settings**
- Repository source list (add/remove/enable/disable URLs).
- Cache management: shows cache size, "Clear Cache" button.
- AIOS version display (for compatibility reference).

### Progress Feedback

Install/update/uninstall operations show a modal dialog with:
- Current stage name ("Downloading...", "Extracting...", etc.)
- Package name being processed
- A progress description (bytes downloaded, file being extracted)
- Cancel button (aborts and cleans up partial state)

### App Menu Integration

After installing or uninstalling a package, the CPM client calls `wm._scan_apps()` (or equivalent) to refresh the desktop's app menu and dock. New apps appear immediately without requiring a reboot.

---

## Terminal Integration

The CPM library is also accessible from the Terminal app as Lua expressions:

```lua
-- In the terminal (these are Lua expressions, not shell commands):
local cpm = require "/system/lib/cpm"

cpm.refresh()
cpm.install("doom")
cpm.uninstall("doom")
cpm.check_updates()
cpm.update_all()
cpm.search("game")
```

**Future:** dedicated terminal commands could be added to the terminal app's command parser for convenience:

```
pkg refresh              -- update package index
pkg install doom         -- install a package
pkg remove doom          -- uninstall a package
pkg update               -- update all packages
pkg search game          -- search packages
pkg list                 -- list installed packages
pkg info doom            -- show package details
```

These would be simple wrappers calling the `cpm` library functions.

---

## Security

### Path Containment (Mandatory)

The single most important security property: **a package cannot write outside its own directory.**

During extraction (Step 4 of the install sequence), every entry path is validated:

```lua
local function is_path_safe(package_name, entry_path)
    -- Reject absolute paths
    if entry_path:sub(1, 1) == "/" then return false end
    -- Reject path traversal
    if entry_path:find("%.%.") then return false end
    -- Compute resolved path
    local target = "/apps/" .. package_name .. "/" .. entry_path
    -- Verify it still starts with the expected prefix
    -- (catches edge cases like embedded null bytes, double slashes, etc.)
    local prefix = "/apps/" .. package_name .. "/"
    return target:sub(1, #prefix) == prefix
end
```

If any entry in a `.cpk` fails this check, **the entire install is aborted** and any already-extracted files from this package are deleted. This is not a warning — it is a hard failure.

### Checksum Verification (Mandatory)

Every downloaded `.cpk` is verified against the CRC-32 checksum in the repository index before extraction. The CRC-32 algorithm is identical to the one used by ChaosFS superblock checksums (polynomial 0xEDB88320, reflected — same as zlib/Ethernet).

If the checksum doesn't match, the downloaded file is deleted and the install fails with a clear error message.

### Trust Model

The current trust model is simple: **you trust the repositories you configure.** The `sources.lua` file is user-controlled. The default is the official AIOS repo on GitHub Pages, which is maintained by the project owner.

This is appropriate for a hobby OS with a single developer and a small community. There is no package signing, no GPG keys, no certificate pinning beyond what BearSSL provides for HTTPS. If the project grows to the point where untrusted third-party repos are common, the following could be added in a future version:

- **HMAC signing:** Each `.cpk` includes an HMAC computed with a shared secret. The repo index includes the expected HMAC. BearSSL already provides HMAC primitives.
- **Index signing:** The `repo.lua` file is accompanied by a `repo.sig` file containing a signature. The client verifies the signature before executing `load()`.
- **Pinned checksums:** A system-level file lists known-good checksums for critical packages, independent of any repo.

These are not needed now and should not be built speculatively.

---

## Disk Space Management

Before downloading a package, CPM checks available disk space:

```lua
local fs = aios.os.fsinfo()
local free_bytes = fs.free_blocks * 4096  -- ChaosFS block size

-- Need space for: cached .cpk + extracted files
-- Worst case: .cpk size + uncompressed size
-- The repo index provides .cpk size. Uncompressed size is not known until
-- extraction, but a 3x expansion ratio is a safe upper bound for LZ4.
local needed = package.size * 4  -- .cpk + ~3x expansion
if free_bytes < needed then
    return nil, string.format("disk full: need ~%d bytes, only %d free",
                              needed, free_bytes)
end
```

This is a conservative estimate. The actual space used will be less because (a) many files in a `.cpk` are stored uncompressed (already small), and (b) the cached `.cpk` can be deleted after extraction. But it's better to refuse early than to fail mid-extraction with a `CHAOS_ERR_NO_SPACE`.

---

## Filesystem Layout Summary

```
/system/
├── lib/
│   └── cpm.lua                 ← CPM core library
├── cpm/
│   ├── installed.lua           ← local package database
│   ├── sources.lua             ← repository URL list
│   └── cache/                  ← downloaded .cpk files
│       └── ...
└── icons/
    └── cpm_32.png              ← CPM app icon

/apps/
├── cpm/                        ← CPM GUI app (pre-installed)
│   ├── manifest.lua
│   └── main.lua
├── doom/                       ← installed via CPM
│   ├── manifest.lua
│   ├── main.lua
│   ├── lib/
│   │   └── ...
│   └── data/                   ← user data (preserved across updates)
│       └── savegame.dat
├── calculator/                 ← installed via CPM
│   ├── manifest.lua
│   └── main.lua
└── editor.lua                  ← legacy single-file app (still works)
```

---

## Dependencies

### Existing Systems Used

| System | Used For | Phase |
|--------|----------|-------|
| `aios.net.*` / `http.lua` | HTTP GET for repo index and `.cpk` downloads | Phase 11 |
| BearSSL | HTTPS/TLS for secure downloads from GitHub Pages | Phase 11 |
| lwIP | TCP/IP stack underlying `aios.net` | Phase 11 |
| `aios.cpk.*` | Opening, listing, and extracting `.cpk` archives | LZ4/CPK |
| LZ4 | Decompression of `.cpk` entries during extraction | LZ4/CPK |
| CRC-32 | Checksum verification of downloaded `.cpk` files | Phase 4 (ChaosFS) |
| `aios.io.*` | All filesystem operations (read, write, mkdir, unlink, rename, stat, listdir) | Phase 4/7 |
| `load()` | Parsing `repo.lua` and `installed.lua` as Lua tables | Phase 7 |
| UI Toolkit | CPM app GUI (TabView, ListView, Button, Dialog, TextField) | Phase 8 |
| WM | App menu refresh after install/uninstall | Phase 9 |
| `tools/cpk_pack.py` | Building `.cpk` files on the host for publishing | LZ4/CPK |

### New Code Required

| Component | Language | Location | Estimated Size |
|-----------|----------|----------|----------------|
| CPM core library | Lua | `/system/lib/cpm.lua` | ~300-400 lines |
| CPM GUI app | Lua | `/apps/cpm/main.lua` | ~200-300 lines |
| Publish tool | Python | `tools/cpk_publish.py` | ~100-150 lines |
| CPM icon | PNG | `/system/icons/cpm_32.png` | asset |

**Total new code: ~600-850 lines of Lua + ~100-150 lines of Python.** No new C code. No new kernel modifications. No new KAOS modules.

---

## Acceptance Tests

1. **Fresh state:** On a system with no `/system/cpm/` directory, `require "/system/lib/cpm"` creates the directory structure and default `sources.lua`. `cpm.installed()` returns an empty table.

2. **Refresh:** `cpm.refresh()` successfully downloads `repo.lua` from the configured source. Returns a table with `packages` array containing at least one entry. Verify each entry has all required fields.

3. **Refresh failure:** With an invalid source URL, `cpm.refresh()` returns the error in the `errors` array but does not crash. If one source fails and another succeeds, packages from the working source are still available.

4. **Install:** `cpm.install("testpkg")` downloads the `.cpk`, verifies checksum, extracts to `/apps/testpkg/`, creates `installed.lua` entry with correct file list. App appears in `aios.io.listdir("/apps")`. `cpm.installed().testpkg` is not nil.

5. **Install checksum mismatch:** Corrupt a cached `.cpk` file. `cpm.install()` detects the CRC-32 mismatch, deletes the corrupt file, and returns an error. No files are extracted.

6. **Install path containment:** Create a test `.cpk` with an entry path containing `../`. `cpm.install()` rejects the entire package. No files are written outside `/apps/<name>/`.

7. **Install dependencies:** Package A depends on package B. `cpm.install("A")` installs B first, then A. Both appear in `installed.lua`.

8. **Install dependency cycle:** Package A depends on B, B depends on A. `cpm.install("A")` fails with a cycle detection error. Neither package is installed.

9. **Duplicate install:** Install a package, then install it again at the same version. Second install returns early (already installed). No re-download.

10. **Uninstall:** `cpm.uninstall("testpkg")` deletes all files from the file list, removes the directory, and removes the entry from `installed.lua`. `cpm.installed().testpkg` is nil.

11. **Uninstall with dependent:** Package A depends on B. `cpm.uninstall("B")` fails with error naming A as a dependent. B remains installed.

12. **Update:** Install version 1.0.0, then `cpm.update("testpkg")` with version 2.0.0 available. Old files are removed, new files are extracted. `installed.lua` shows version 2.0.0.

13. **Update preserves data:** Create `/apps/testpkg/data/save.txt` after install. `cpm.update("testpkg")` replaces app files but `/apps/testpkg/data/save.txt` survives.

14. **Check updates:** Install an old version. `cpm.check_updates()` returns an array with one entry showing the available update.

15. **Search:** `cpm.search("calc")` returns packages whose name or description contains "calc". Case-insensitive.

16. **Disk space check:** With a nearly full disk, `cpm.install()` for a large package fails with a disk space error before attempting download.

17. **Cache reuse:** Install a package (populates cache). Uninstall it. Install again — the second install uses the cached `.cpk` (no HTTP request if checksum matches).

18. **Clear cache:** After installing several packages, cache directory contains `.cpk` files. After clearing cache, directory is empty. Installed packages are unaffected.

19. **App menu refresh:** After `cpm.install()`, the WM's app list includes the new app without a reboot. After `cpm.uninstall()`, it's gone.

20. **Atomic database write:** Kill the CPM process during an install (simulate crash). On next boot, `installed.lua` is either the old version (if crash before rename) or the new version (if crash after rename). Never corrupt.

---

## Summary

| Property | Value |
|----------|-------|
| Server | Static files on GitHub Pages (free, HTTPS) |
| Protocol | HTTPS GET (BearSSL + lwIP) |
| Index format | Lua table (`repo.lua`) |
| Package format | `.cpk` (LZ4-compressed archive with CRC-32) |
| Local database | Lua table (`installed.lua`) |
| Install location | `/apps/<package-name>/` (enforced) |
| Version scheme | MAJOR.MINOR.PATCH (numeric comparison) |
| Dependency resolution | Recursive depth-first with cycle detection |
| User data preservation | `/apps/<name>/data/` survives updates |
| Security | Path containment + CRC-32 verification + HTTPS transport |
| New C code | None |
| New Lua code | ~600-850 lines |
| New Python code | ~100-150 lines |

**The entire package manager is built from APIs that already exist.** The networking stack downloads files. The CPK system extracts archives. The filesystem stores everything. CRC-32 verifies integrity. Lua tables are the data format. The CPM spec is pure orchestration — connecting existing pieces in the right order with the right safety checks.
