#!/usr/bin/env python3
"""cpk_pack.py — Create .cpk (Chaos Package) archives from directories.

Usage: python cpk_pack.py <input_dir> <output.cpk> [--lz4]

Creates a CPK archive containing all files in <input_dir> with relative paths.
Optional --lz4 flag enables per-file LZ4 block compression.
"""

import struct
import sys
import os

# ═══════════════════════════════════════════════════════════════════════
# Constants
# ═══════════════════════════════════════════════════════════════════════

CPK_MAGIC   = 0x43504B47  # 'CPKG'
CPK_VERSION = 1
CPK_FLAG_LZ4 = 1

CPK_HEADER_SIZE = 32   # struct cpk_header
CPK_ENTRY_SIZE  = 144  # struct cpk_entry

# ═══════════════════════════════════════════════════════════════════════
# CRC-32 (matches kernel chaos_crc32)
# ═══════════════════════════════════════════════════════════════════════

def crc32_chaos(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1))
    return (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF

# ═══════════════════════════════════════════════════════════════════════
# Minimal LZ4 Block Compressor (pure Python)
# ═══════════════════════════════════════════════════════════════════════

def lz4_compress(src):
    """Compress bytes using LZ4 block format. Returns compressed bytes."""
    src_len = len(src)
    if src_len < 5:
        return _lz4_emit_literals(src)

    out = bytearray()
    htable = {}  # hash -> position
    anchor = 0
    ip = 1

    while ip <= src_len - 4:
        # Hash 4 bytes
        h = _lz4_hash(src[ip:ip+4])
        ref = htable.get(h, -1)
        htable[h] = ip

        # Check match
        if ref < 0 or ip - ref > 65535 or src[ref:ref+4] != src[ip:ip+4]:
            ip += 1
            continue

        # Extend match
        match_end = ip + 4
        ref_end = ref + 4
        while match_end < src_len and ref_end < ip and src[match_end] == src[ref_end]:
            match_end += 1
            ref_end += 1

        lit_len = ip - anchor
        match_len = match_end - ip - 4  # minus minmatch
        offset = ip - ref

        # Token
        token = min(lit_len, 15) << 4 | min(match_len, 15)
        out.append(token)

        # Extra literal length
        if lit_len >= 15:
            _lz4_write_vlen(out, lit_len - 15)

        # Literals
        out.extend(src[anchor:ip])

        # Offset (little-endian 16-bit)
        out.append(offset & 0xFF)
        out.append((offset >> 8) & 0xFF)

        # Extra match length
        if match_len >= 15:
            _lz4_write_vlen(out, match_len - 15)

        ip = match_end
        anchor = ip

        # Update hash for skipped positions
        if ip <= src_len - 4 and ip >= 2:
            htable[_lz4_hash(src[ip-2:ip+2])] = ip - 2

    # Final literals
    last = src[anchor:]
    if last:
        lit_len = len(last)
        token = min(lit_len, 15) << 4
        out.append(token)
        if lit_len >= 15:
            _lz4_write_vlen(out, lit_len - 15)
        out.extend(last)

    return bytes(out)


def _lz4_hash(data):
    v = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24)
    return ((v * 2654435761) & 0xFFFFFFFF) >> 20  # 12-bit hash


def _lz4_write_vlen(out, length):
    while length >= 255:
        out.append(255)
        length -= 255
    out.append(length)


def _lz4_emit_literals(src):
    out = bytearray()
    lit_len = len(src)
    token = min(lit_len, 15) << 4
    out.append(token)
    if lit_len >= 15:
        _lz4_write_vlen(out, lit_len - 15)
    out.extend(src)
    return bytes(out)

# ═══════════════════════════════════════════════════════════════════════
# CPK Archive Writer
# ═══════════════════════════════════════════════════════════════════════

def collect_files(input_dir):
    """Walk directory, return list of (relative_path, full_path) tuples."""
    files = []
    for root, dirs, filenames in os.walk(input_dir):
        # Sort for deterministic output
        dirs.sort()
        filenames.sort()
        for fname in filenames:
            full = os.path.join(root, fname)
            rel = os.path.relpath(full, input_dir).replace('\\', '/')
            files.append((rel, full))
    return files


def create_cpk(input_dir, output_path, use_lz4=False):
    """Create a .cpk archive from a directory."""
    files = collect_files(input_dir)
    if not files:
        print(f"cpk_pack: no files found in {input_dir}")
        return False

    flags = CPK_FLAG_LZ4 if use_lz4 else 0

    # Phase 1: Read and optionally compress all files
    entries = []
    data_blocks = []
    offset = CPK_HEADER_SIZE  # data starts right after header

    for rel_path, full_path in files:
        with open(full_path, 'rb') as f:
            original = f.read()

        crc = crc32_chaos(original)
        size_original = len(original)

        if use_lz4 and size_original > 16:
            compressed = lz4_compress(original)
            # Only use compressed version if it's actually smaller
            if len(compressed) < size_original:
                data = compressed
            else:
                data = original
        else:
            data = original

        size_compressed = len(data)

        # Truncate path to 127 chars (+ null)
        if len(rel_path) > 127:
            print(f"  WARNING: path truncated: {rel_path}")
            rel_path = rel_path[:127]

        entries.append({
            'path': rel_path,
            'offset': offset,
            'size_compressed': size_compressed,
            'size_original': size_original,
            'checksum': crc,
        })
        data_blocks.append(data)
        offset += size_compressed

    toc_offset = offset

    # Phase 2: Write archive
    with open(output_path, 'wb') as f:
        # Header (32 bytes)
        header = struct.pack('<IIIII12s',
            CPK_MAGIC, CPK_VERSION, len(entries), toc_offset, flags,
            b'\x00' * 12)
        f.write(header)

        # File data
        for data in data_blocks:
            f.write(data)

        # TOC
        for e in entries:
            path_bytes = e['path'].encode('ascii')
            path_padded = path_bytes + b'\x00' * (128 - len(path_bytes))
            entry = struct.pack('<128sIIII',
                path_padded,
                e['offset'],
                e['size_compressed'],
                e['size_original'],
                e['checksum'])
            f.write(entry)

    total_original = sum(e['size_original'] for e in entries)
    total_compressed = sum(e['size_compressed'] for e in entries)
    archive_size = toc_offset + len(entries) * CPK_ENTRY_SIZE

    print(f"cpk_pack: {output_path}")
    print(f"  {len(entries)} files, {total_original} bytes original")
    if use_lz4:
        ratio = total_compressed / total_original * 100 if total_original > 0 else 100
        print(f"  {total_compressed} bytes compressed ({ratio:.1f}%)")
    print(f"  archive size: {archive_size} bytes")
    return True


# ═══════════════════════════════════════════════════════════════════════
# CLI
# ═══════════════════════════════════════════════════════════════════════

def main():
    if len(sys.argv) < 3:
        print("Usage: python cpk_pack.py <input_dir> <output.cpk> [--lz4]")
        sys.exit(1)

    input_dir = sys.argv[1]
    output_path = sys.argv[2]
    use_lz4 = '--lz4' in sys.argv

    if not os.path.isdir(input_dir):
        print(f"Error: {input_dir} is not a directory")
        sys.exit(1)

    if not create_cpk(input_dir, output_path, use_lz4):
        sys.exit(1)


if __name__ == '__main__':
    main()
