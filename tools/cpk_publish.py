#!/usr/bin/env python3
"""cpk_publish.py — Package AIOS apps and generate repository index.

Usage: python cpk_publish.py <apps_dir> <repo_dir> [--lz4]

Reads all apps from <apps_dir> (each subdirectory with manifest.lua),
packages them as .cpk archives, and generates repo.lua index.
"""

import os
import sys
import re
import struct
import subprocess

def crc32_chaos(data):
    """CRC-32 matching kernel chaos_crc32 (polynomial 0xEDB88320, reflected)."""
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1))
    return (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF

def parse_manifest(manifest_path):
    """Parse a Lua manifest.lua file into a dict."""
    with open(manifest_path, 'r') as f:
        content = f.read()

    result = {}
    for key in ['name', 'version', 'author', 'description', 'icon', 'entry']:
        m = re.search(rf'{key}\s*=\s*"([^"]*)"', content)
        if m:
            result[key] = m.group(1)
    return result

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <apps_dir> <repo_dir> [--lz4]")
        sys.exit(1)

    apps_dir = sys.argv[1]
    repo_dir = sys.argv[2]
    use_lz4 = "--lz4" in sys.argv

    # Create repo structure
    packages_dir = os.path.join(repo_dir, "packages")
    os.makedirs(packages_dir, exist_ok=True)

    # Find cpk_pack.py
    script_dir = os.path.dirname(os.path.abspath(__file__))
    cpk_pack = os.path.join(script_dir, "cpk_pack.py")
    if not os.path.exists(cpk_pack):
        print(f"Error: cpk_pack.py not found at {cpk_pack}")
        sys.exit(1)

    packages = []

    # Scan apps directory
    for entry in sorted(os.listdir(apps_dir)):
        app_dir = os.path.join(apps_dir, entry)
        manifest_path = os.path.join(app_dir, "manifest.lua")

        if not os.path.isdir(app_dir) or not os.path.exists(manifest_path):
            continue

        manifest = parse_manifest(manifest_path)
        if 'name' not in manifest or 'version' not in manifest:
            print(f"  Skipping {entry}: missing name or version in manifest")
            continue

        version = manifest['version']
        # Normalize version to X.Y.Z
        if version.count('.') == 1:
            version += '.0'

        cpk_name = f"{entry}-{version}.cpk"
        cpk_path = os.path.join(packages_dir, cpk_name)

        # Run cpk_pack.py
        cmd = [sys.executable, cpk_pack, app_dir, cpk_path]
        if use_lz4:
            cmd.append("--lz4")

        print(f"  Packaging {entry} v{version}...")
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"    Error: {result.stderr.strip()}")
            continue

        # Compute CRC-32 and size
        with open(cpk_path, 'rb') as f:
            cpk_data = f.read()

        checksum = crc32_chaos(cpk_data)
        size = len(cpk_data)

        print(f"    {cpk_name}: {size} bytes, CRC-32=0x{checksum:08X}")

        packages.append({
            'name': entry,
            'version': version,
            'author': manifest.get('author', 'AIOS'),
            'description': manifest.get('description', ''),
            'icon': manifest.get('icon', ''),
            'size': size,
            'checksum': checksum,
            'url': f"packages/{cpk_name}",
            'category': 'system',  # built-in apps
        })

    # Generate repo.lua
    repo_lua = 'return {\n'
    repo_lua += '    repo_version = 1,\n'
    repo_lua += '    repo_name = "AIOS Official",\n'
    repo_lua += f'    updated = "{__import__("datetime").date.today().isoformat()}",\n'
    repo_lua += '    packages = {\n'

    for pkg in packages:
        repo_lua += '        {\n'
        repo_lua += f'            name        = "{pkg["name"]}",\n'
        repo_lua += f'            version     = "{pkg["version"]}",\n'
        repo_lua += f'            author      = "{pkg["author"]}",\n'
        repo_lua += f'            description = "{pkg["description"]}",\n'
        if pkg['icon']:
            repo_lua += f'            icon        = "{pkg["icon"]}",\n'
        repo_lua += f'            size        = {pkg["size"]},\n'
        repo_lua += f'            checksum    = 0x{pkg["checksum"]:08X},\n'
        repo_lua += f'            url         = "{pkg["url"]}",\n'
        repo_lua += '            depends     = {},\n'
        repo_lua += '            min_aios    = "2.0",\n'
        repo_lua += f'            category    = "{pkg["category"]}",\n'
        repo_lua += '        },\n'

    repo_lua += '    },\n'
    repo_lua += '}\n'

    repo_path = os.path.join(repo_dir, "repo.lua")
    with open(repo_path, 'w') as f:
        f.write(repo_lua)

    print(f"\nGenerated {repo_path} with {len(packages)} package(s)")
    print(f"Repository ready at: {repo_dir}")

if __name__ == "__main__":
    main()
