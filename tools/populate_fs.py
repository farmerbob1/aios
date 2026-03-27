#!/usr/bin/env python3
"""populate_fs — Format ChaosFS and populate it with all content.

Unified tool that replaces mkfs_chaos.py + gen_assets.py + gen_modules.py.
Handles: formatting, static files from harddrive/, compiled .kaos modules,
and programmatically generated test assets.

Usage: python populate_fs.py <disk_image> <lba_start> [modules_dir]
"""

import struct
import sys
import os
import math
import zlib

# ═══════════════════════════════════════════════════════════════════════
# Constants (must match chaos_types.h)
# ═══════════════════════════════════════════════════════════════════════

CHAOS_MAGIC = 0x43484653
CHAOS_VERSION = 2
CHAOS_BLOCK_SIZE = 4096
CHAOS_SECTORS_PER_BLK = 8
CHAOS_INODE_MAGIC = 0xC4A0
CHAOS_INODE_SIZE = 128
CHAOS_INODES_PER_BLOCK = CHAOS_BLOCK_SIZE // CHAOS_INODE_SIZE  # 32
CHAOS_DIRENT_SIZE = 64
CHAOS_TYPE_DIR = 0x2000
CHAOS_TYPE_FILE = 0x8000
CHAOS_DT_DIR = 2
CHAOS_DT_REG = 1

RAW_MAGIC = 0x52415754   # 'RAWT'
COBJ_MAGIC = 0x434F424A

ROOT_INO = 1

# ═══════════════════════════════════════════════════════════════════════
# Low-level disk I/O
# ═══════════════════════════════════════════════════════════════════════

def block_offset(lba_start, block_idx):
    return (lba_start + block_idx * CHAOS_SECTORS_PER_BLK) * 512

def read_block(f, lba_start, block_idx):
    f.seek(block_offset(lba_start, block_idx))
    return bytearray(f.read(CHAOS_BLOCK_SIZE))

def write_block(f, lba_start, block_idx, data):
    assert len(data) == CHAOS_BLOCK_SIZE
    f.seek(block_offset(lba_start, block_idx))
    f.write(data)

def crc32_chaos(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1))
    return crc ^ 0xFFFFFFFF

# ═══════════════════════════════════════════════════════════════════════
# Format (replaces mkfs_chaos.py)
# ═══════════════════════════════════════════════════════════════════════

def make_superblock(total_blocks, bitmap_blocks, inode_count, inode_table_blocks, data_start):
    sb = bytearray(CHAOS_BLOCK_SIZE)
    offset = 0
    def pack(fmt, *args):
        nonlocal offset
        struct.pack_into(fmt, sb, offset, *args)
        offset += struct.calcsize(fmt)

    pack('<I', CHAOS_MAGIC)
    pack('<I', CHAOS_VERSION)
    pack('16s', b'AIOS\x00' + b'\x00' * 11)
    pack('<I', CHAOS_BLOCK_SIZE)
    pack('<I', total_blocks)
    pack('<I', 2)                            # bitmap_start
    pack('<I', bitmap_blocks)
    pack('<I', 2 + bitmap_blocks)            # inode_table_start
    pack('<I', inode_table_blocks)
    pack('<I', data_start)
    pack('<I', inode_count)
    pack('<I', total_blocks - data_start - 1) # free_blocks
    pack('<I', inode_count - 2)              # free_inodes
    pack('<B', 1)                            # clean_unmount
    pack('<B', 0)                            # mounted
    pack('<H', 0)                            # mount_count
    pack('<I', 0)                            # created_time
    pack('<I', 0)                            # last_mounted_time
    pack('<I', 0)                            # last_fsck_time
    pack('<I', 0)                            # journal_start
    pack('<I', 0)                            # journal_blocks
    offset += 28                             # reserved[28]

    checksum_offset = offset
    crc = crc32_chaos(sb[:checksum_offset])
    struct.pack_into('<I', sb, checksum_offset, crc)
    return bytes(sb)

def make_inode(mode, link_count, size, extent_start, extent_blocks):
    ino = bytearray(CHAOS_INODE_SIZE)
    struct.pack_into('<H', ino, 0, CHAOS_INODE_MAGIC)
    struct.pack_into('<H', ino, 2, mode)
    struct.pack_into('<I', ino, 4, link_count)
    struct.pack_into('<Q', ino, 16, size)
    struct.pack_into('<B', ino, 40, 1)
    struct.pack_into('<I', ino, 44, extent_start)
    struct.pack_into('<I', ino, 48, extent_blocks)
    return bytes(ino)

def make_dirent(inode_num, dtype, name):
    d = bytearray(CHAOS_DIRENT_SIZE)
    name_bytes = name.encode('ascii')
    struct.pack_into('<I', d, 0, inode_num)
    struct.pack_into('<B', d, 4, dtype)
    struct.pack_into('<B', d, 5, len(name_bytes))
    d[6:6 + len(name_bytes)] = name_bytes
    return bytes(d)

def format_chaosfs(f, lba_start):
    """Format ChaosFS at lba_start. Returns (total_blocks, data_start, bitmap_blocks, inode_table_blocks, inode_count)."""
    file_size = f.seek(0, 2)
    fs_offset = lba_start * 512
    lba_count = (file_size - fs_offset) // 512
    total_blocks = (lba_count * 512) // CHAOS_BLOCK_SIZE

    bitmap_blocks = (total_blocks + 32767) // 32768
    inode_count = max(total_blocks // 4, 32)
    inode_table_blocks = (inode_count + CHAOS_INODES_PER_BLOCK - 1) // CHAOS_INODES_PER_BLOCK
    data_start = 2 + bitmap_blocks + inode_table_blocks

    print(f"populate_fs: format — {total_blocks} blocks, bitmap={bitmap_blocks}, "
          f"inodes={inode_count} ({inode_table_blocks} blocks), data_start={data_start}")

    zero_block = b'\x00' * CHAOS_BLOCK_SIZE
    for b in range(data_start):
        write_block(f, lba_start, b, zero_block)

    # Bitmap: mark metadata + root data block as used
    bitmap_bytes = bytearray(bitmap_blocks * CHAOS_BLOCK_SIZE)
    for b in range(data_start):
        bitmap_bytes[b // 8] |= (1 << (b % 8))
    root_data = data_start
    bitmap_bytes[root_data // 8] |= (1 << (root_data % 8))

    for i in range(bitmap_blocks):
        chunk = bitmap_bytes[i * CHAOS_BLOCK_SIZE:(i + 1) * CHAOS_BLOCK_SIZE]
        write_block(f, lba_start, 2 + i, bytes(chunk))

    # Root inode (inode 1)
    inode_block = bytearray(CHAOS_BLOCK_SIZE)
    root_ino = make_inode(CHAOS_TYPE_DIR | 0o755, 2, CHAOS_BLOCK_SIZE, root_data, 1)
    inode_block[CHAOS_INODE_SIZE:CHAOS_INODE_SIZE * 2] = root_ino
    write_block(f, lba_start, 2 + bitmap_blocks, bytes(inode_block))

    # Root directory data (. and ..)
    dir_block = bytearray(CHAOS_BLOCK_SIZE)
    dir_block[0:CHAOS_DIRENT_SIZE] = make_dirent(1, CHAOS_DT_DIR, ".")
    dir_block[CHAOS_DIRENT_SIZE:CHAOS_DIRENT_SIZE * 2] = make_dirent(1, CHAOS_DT_DIR, "..")
    write_block(f, lba_start, root_data, bytes(dir_block))

    # Superblock (primary + backup)
    sb = make_superblock(total_blocks, bitmap_blocks, inode_count, inode_table_blocks, data_start)
    write_block(f, lba_start, 0, sb)
    write_block(f, lba_start, 1, sb)

    return total_blocks, data_start, bitmap_blocks, inode_table_blocks, inode_count

# ═══════════════════════════════════════════════════════════════════════
# ChaosFS Writer (high-level file/dir creation)
# ═══════════════════════════════════════════════════════════════════════

class Superblock:
    def __init__(self, raw):
        (self.magic, self.version) = struct.unpack_from('<II', raw, 0)
        self.fs_name = raw[8:24]
        (self.block_size,) = struct.unpack_from('<I', raw, 24)
        (self.total_blocks,) = struct.unpack_from('<I', raw, 28)
        (self.bitmap_start,) = struct.unpack_from('<I', raw, 32)
        (self.bitmap_blocks,) = struct.unpack_from('<I', raw, 36)
        (self.inode_table_start,) = struct.unpack_from('<I', raw, 40)
        (self.inode_table_blocks,) = struct.unpack_from('<I', raw, 44)
        (self.data_start,) = struct.unpack_from('<I', raw, 48)
        (self.total_inodes,) = struct.unpack_from('<I', raw, 52)
        (self.free_blocks,) = struct.unpack_from('<I', raw, 56)
        (self.free_inodes,) = struct.unpack_from('<I', raw, 60)
        self.raw = bytearray(raw)

    def set_free_blocks(self, n):
        self.free_blocks = n
        struct.pack_into('<I', self.raw, 56, n)

    def set_free_inodes(self, n):
        self.free_inodes = n
        struct.pack_into('<I', self.raw, 60, n)

    def recompute_crc(self):
        checksum_offset = 116
        crc = crc32_chaos(self.raw[:checksum_offset])
        struct.pack_into('<I', self.raw, checksum_offset, crc)

    def to_bytes(self):
        self.recompute_crc()
        return bytes(self.raw)


class Bitmap:
    def __init__(self, data):
        self.data = bytearray(data)

    def is_used(self, bit):
        return bool(self.data[bit // 8] & (1 << (bit % 8)))

    def set_used(self, bit):
        self.data[bit // 8] |= (1 << (bit % 8))

    def alloc_contiguous(self, count, search_start):
        run_start = search_start
        run_len = 0
        limit = len(self.data) * 8
        for b in range(search_start, limit):
            if self.is_used(b):
                run_start = b + 1
                run_len = 0
            else:
                run_len += 1
                if run_len == count:
                    for i in range(run_start, run_start + count):
                        self.set_used(i)
                    return run_start
        raise RuntimeError(f"Cannot allocate {count} contiguous blocks")


class ChaosFSWriter:
    def __init__(self, f, lba_start):
        self.f = f
        self.lba_start = lba_start

        sb_raw = read_block(f, lba_start, 0)
        self.sb = Superblock(sb_raw)
        assert self.sb.magic == CHAOS_MAGIC, f"Bad magic: 0x{self.sb.magic:08X}"

        bmp_data = bytearray()
        for i in range(self.sb.bitmap_blocks):
            bmp_data += read_block(f, lba_start, self.sb.bitmap_start + i)
        self.bitmap = Bitmap(bmp_data)

        self.next_inode_search = 2
        self.blocks_allocated = 0
        self.inodes_allocated = 0
        self.files_written = 0
        self.dirs_created = 0

    def alloc_inode(self):
        for ino_num in range(self.next_inode_search, self.sb.total_inodes):
            tbl_block = self.sb.inode_table_start + (ino_num // CHAOS_INODES_PER_BLOCK)
            slot = ino_num % CHAOS_INODES_PER_BLOCK
            blk = read_block(self.f, self.lba_start, tbl_block)
            off = slot * CHAOS_INODE_SIZE
            (magic,) = struct.unpack_from('<H', blk, off)
            if magic == 0:
                self.next_inode_search = ino_num + 1
                self.inodes_allocated += 1
                return ino_num
        raise RuntimeError("No free inodes")

    def write_inode(self, ino_num, inode_data):
        tbl_block = self.sb.inode_table_start + (ino_num // CHAOS_INODES_PER_BLOCK)
        slot = ino_num % CHAOS_INODES_PER_BLOCK
        blk = read_block(self.f, self.lba_start, tbl_block)
        off = slot * CHAOS_INODE_SIZE
        blk[off:off + CHAOS_INODE_SIZE] = inode_data
        write_block(self.f, self.lba_start, tbl_block, bytes(blk))

    def alloc_blocks(self, count):
        start = self.bitmap.alloc_contiguous(count, self.sb.data_start)
        self.blocks_allocated += count
        return start

    def write_file_data(self, start_block, data):
        num_blocks = (len(data) + CHAOS_BLOCK_SIZE - 1) // CHAOS_BLOCK_SIZE
        padded = bytearray(data)
        padded += b'\x00' * (num_blocks * CHAOS_BLOCK_SIZE - len(data))
        for i in range(num_blocks):
            chunk = padded[i * CHAOS_BLOCK_SIZE:(i + 1) * CHAOS_BLOCK_SIZE]
            write_block(self.f, self.lba_start, start_block + i, bytes(chunk))

    def add_dirent_to_dir(self, dir_ino_num, child_ino, dtype, name):
        tbl_block = self.sb.inode_table_start + (dir_ino_num // CHAOS_INODES_PER_BLOCK)
        slot = dir_ino_num % CHAOS_INODES_PER_BLOCK
        ino_blk = read_block(self.f, self.lba_start, tbl_block)
        off = slot * CHAOS_INODE_SIZE
        (extent_start,) = struct.unpack_from('<I', ino_blk, off + 44)

        dir_data = read_block(self.f, self.lba_start, extent_start)
        new_off = -1
        max_entries = CHAOS_BLOCK_SIZE // CHAOS_DIRENT_SIZE
        for i in range(max_entries):
            entry_off = i * CHAOS_DIRENT_SIZE
            (ino,) = struct.unpack_from('<I', dir_data, entry_off)
            if ino == 0:
                new_off = entry_off
                break

        if new_off < 0:
            raise RuntimeError(f"Directory inode {dir_ino_num} is full")

        entry = make_dirent(child_ino, dtype, name)
        dir_data[new_off:new_off + CHAOS_DIRENT_SIZE] = entry
        write_block(self.f, self.lba_start, extent_start, bytes(dir_data))

    def create_directory(self, parent_ino, name):
        ino_num = self.alloc_inode()
        data_block = self.alloc_blocks(1)

        dir_data = bytearray(CHAOS_BLOCK_SIZE)
        dir_data[0:CHAOS_DIRENT_SIZE] = make_dirent(ino_num, CHAOS_DT_DIR, ".")
        dir_data[CHAOS_DIRENT_SIZE:CHAOS_DIRENT_SIZE * 2] = make_dirent(parent_ino, CHAOS_DT_DIR, "..")
        write_block(self.f, self.lba_start, data_block, bytes(dir_data))

        dir_size = CHAOS_DIRENT_SIZE * 2
        inode = make_inode(CHAOS_TYPE_DIR | 0o755, 2, dir_size, data_block, 1)
        self.write_inode(ino_num, inode)
        self.add_dirent_to_dir(parent_ino, ino_num, CHAOS_DT_DIR, name)
        self.dirs_created += 1
        return ino_num

    def create_file(self, parent_ino, name, data):
        ino_num = self.alloc_inode()
        num_blocks = max(1, (len(data) + CHAOS_BLOCK_SIZE - 1) // CHAOS_BLOCK_SIZE)
        start_block = self.alloc_blocks(num_blocks)

        self.write_file_data(start_block, data)
        inode = make_inode(CHAOS_TYPE_FILE | 0o644, 1, len(data), start_block, num_blocks)
        self.write_inode(ino_num, inode)
        self.add_dirent_to_dir(parent_ino, ino_num, CHAOS_DT_REG, name)
        self.files_written += 1
        return ino_num

    def ensure_directory(self, path):
        """Create directory path recursively. Returns inode of deepest dir."""
        parts = [p for p in path.strip('/').split('/') if p]
        current_ino = ROOT_INO
        for part in parts:
            # Check if dir already exists by scanning dirents
            existing = self._find_dirent(current_ino, part)
            if existing is not None:
                current_ino = existing
            else:
                current_ino = self.create_directory(current_ino, part)
        return current_ino

    def _find_dirent(self, dir_ino, name):
        """Find a dirent by name in a directory. Returns inode or None."""
        tbl_block = self.sb.inode_table_start + (dir_ino // CHAOS_INODES_PER_BLOCK)
        slot = dir_ino % CHAOS_INODES_PER_BLOCK
        ino_blk = read_block(self.f, self.lba_start, tbl_block)
        off = slot * CHAOS_INODE_SIZE
        (extent_start,) = struct.unpack_from('<I', ino_blk, off + 44)

        dir_data = read_block(self.f, self.lba_start, extent_start)
        max_entries = CHAOS_BLOCK_SIZE // CHAOS_DIRENT_SIZE
        for i in range(max_entries):
            entry_off = i * CHAOS_DIRENT_SIZE
            (ino,) = struct.unpack_from('<I', dir_data, entry_off)
            if ino == 0:
                continue
            (name_len,) = struct.unpack_from('<B', dir_data, entry_off + 5)
            entry_name = dir_data[entry_off + 6:entry_off + 6 + name_len].decode('ascii')
            if entry_name == name:
                return ino
        return None

    def flush(self):
        for i in range(self.sb.bitmap_blocks):
            chunk = self.bitmap.data[i * CHAOS_BLOCK_SIZE:(i + 1) * CHAOS_BLOCK_SIZE]
            write_block(self.f, self.lba_start, self.sb.bitmap_start + i, bytes(chunk))

        self.sb.set_free_blocks(self.sb.free_blocks - self.blocks_allocated)
        self.sb.set_free_inodes(self.sb.free_inodes - self.inodes_allocated)

        sb_bytes = self.sb.to_bytes()
        write_block(self.f, self.lba_start, 0, sb_bytes)
        write_block(self.f, self.lba_start, 1, sb_bytes)

# ═══════════════════════════════════════════════════════════════════════
# Test asset generators (from gen_assets.py)
# ═══════════════════════════════════════════════════════════════════════

def make_raw_header(width, height):
    return struct.pack('<IIII', RAW_MAGIC, width, height, 0)

def _png_chunk(chunk_type, data):
    """Build a PNG chunk: length + type + data + CRC32."""
    chunk = chunk_type + data
    return struct.pack('>I', len(data)) + chunk + struct.pack('>I', zlib.crc32(chunk) & 0xFFFFFFFF)

def gen_test_png():
    """Generate a small 16x16 RGBA PNG with alpha gradient for testing.
    Red/green checkerboard pattern with varying alpha."""
    w, h = 16, 16
    raw_rows = bytearray()
    for y in range(h):
        raw_rows.append(0)  # filter byte: None
        for x in range(w):
            if (x + y) % 2 == 0:
                r, g, b = 255, 0, 0    # red
            else:
                r, g, b = 0, 255, 0    # green
            a = int(255 * (x + 1) / w)  # alpha gradient left-to-right
            raw_rows.extend([r, g, b, a])
    # PNG signature
    sig = b'\x89PNG\r\n\x1a\n'
    # IHDR: width, height, bit_depth=8, color_type=6 (RGBA), compress=0, filter=0, interlace=0
    ihdr_data = struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)
    # IDAT: deflate-compressed raw image data
    idat_data = zlib.compress(bytes(raw_rows))
    return sig + _png_chunk(b'IHDR', ihdr_data) + _png_chunk(b'IDAT', idat_data) + _png_chunk(b'IEND', b'')

# ═══════════════════════════════════════════════════════════════════════
# ChaosRIP game asset generators
# ═══════════════════════════════════════════════════════════════════════

def _make_png_rgb(w, h, pixels):
    """Create an RGB PNG from a flat pixel list [(r,g,b), ...]."""
    raw_rows = bytearray()
    for y in range(h):
        raw_rows.append(0)  # filter: None
        for x in range(w):
            r, g, b = pixels[y * w + x]
            raw_rows.extend([r, g, b])
    sig = b'\x89PNG\r\n\x1a\n'
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)  # color_type=2 (RGB)
    idat = zlib.compress(bytes(raw_rows))
    return sig + _png_chunk(b'IHDR', ihdr) + _png_chunk(b'IDAT', idat) + _png_chunk(b'IEND', b'')

def _make_png_rgba(w, h, pixels):
    """Create an RGBA PNG from [(r,g,b,a), ...]."""
    raw_rows = bytearray()
    for y in range(h):
        raw_rows.append(0)
        for x in range(w):
            raw_rows.extend(pixels[y * w + x])
    sig = b'\x89PNG\r\n\x1a\n'
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)
    idat = zlib.compress(bytes(raw_rows))
    return sig + _png_chunk(b'IHDR', ihdr) + _png_chunk(b'IDAT', idat) + _png_chunk(b'IEND', b'')

def _gen_tiled_tex(w, h, tile_w, tile_h, color1, color2, grout=None):
    """Generate a tiled texture pattern."""
    px = []
    for y in range(h):
        for x in range(w):
            tx = x % tile_w
            ty = y % tile_h
            if grout and (tx == 0 or ty == 0):
                px.append(grout)
            elif ((x // tile_w) + (y // tile_h)) % 2 == 0:
                px.append(color1)
            else:
                px.append(color2)
    return px

def gen_rip_brick1(w=128, h=128):
    """Brown/red brick wall texture."""
    px = []
    for y in range(h):
        for x in range(w):
            row = y % 32
            col = x % 64
            offset = 32 if (y // 32) % 2 else 0
            bx = (x + offset) % 64
            if row < 2 or bx < 2:
                px.append((80, 70, 60))  # grout
            else:
                v = 120 + ((x * 7 + y * 13) % 40)
                px.append((v, int(v * 0.45), int(v * 0.3)))
    return _make_png_rgb(w, h, px)

def gen_rip_brick2(w=128, h=128):
    """Grey stone brick."""
    px = []
    for y in range(h):
        for x in range(w):
            row = y % 32
            col = x % 64
            offset = 32 if (y // 32) % 2 else 0
            bx = (x + offset) % 64
            if row < 2 or bx < 2:
                px.append((50, 50, 50))
            else:
                v = 100 + ((x * 11 + y * 7) % 50)
                px.append((v, v, int(v * 0.95)))
    return _make_png_rgb(w, h, px)

def gen_rip_metal1(w=128, h=128):
    """Metal plate with rivets."""
    px = []
    for y in range(h):
        for x in range(w):
            v = 130 + ((x * 3 + y * 5) % 30)
            if (x % 32 < 2 or y % 32 < 2):
                px.append((int(v * 0.6), int(v * 0.6), int(v * 0.7)))
            elif (x % 32 in [4, 5] and y % 32 in [4, 5]):
                px.append((180, 180, 200))  # rivet
            else:
                px.append((int(v * 0.7), int(v * 0.75), int(v * 0.85)))
    return _make_png_rgb(w, h, px)

def gen_rip_floor1(w=128, h=128):
    """Stone floor tiles."""
    px = _gen_tiled_tex(w, h, 32, 32, (100, 95, 85), (90, 85, 75), grout=(60, 55, 50))
    return _make_png_rgb(w, h, px)

def gen_rip_ceil1(w=128, h=128):
    """Ceiling panels."""
    px = _gen_tiled_tex(w, h, 64, 64, (120, 115, 110), (110, 105, 100), grout=(80, 75, 70))
    return _make_png_rgb(w, h, px)

def gen_rip_door1(w=128, h=128):
    """Door texture."""
    px = []
    for y in range(h):
        for x in range(w):
            if x < 4 or x >= 124 or y < 4 or y >= 124:
                px.append((60, 60, 70))  # frame
            elif x < 8 or x >= 120 or y < 8 or y >= 120:
                px.append((100, 100, 120))  # inner frame
            else:
                v = 80 + ((y * 3) % 30)
                px.append((int(v * 0.5), int(v * 0.5), v))  # blue-ish door
    return _make_png_rgb(w, h, px)

def gen_rip_sky1(w=320, h=160):
    """Sky gradient with dots for stars."""
    import random
    rng = random.Random(42)
    px = []
    for y in range(h):
        t = y / h
        r = int(40 + 20 * t)
        g = int(40 + 30 * t)
        b = int(80 - 20 * t)
        for x in range(w):
            if rng.random() < 0.003 and t < 0.5:
                px.append((200, 200, 220))
            else:
                px.append((r, g, b))
    return _make_png_rgb(w, h, px)

def gen_rip_imp_spritesheet(w=512, h=512):
    """Imp sprite sheet: 8x8 grid of 64x64 cells.
    Rows: idle, walk1, walk2, attack1, attack2, pain, death1, death2.
    Cols: 8 rotation angles.
    Colored humanoid silhouettes on magenta background."""
    MAGENTA = (255, 0, 255)
    state_colors = [
        (0, 180, 0),    # idle - green
        (0, 150, 50),   # walk1
        (0, 160, 30),   # walk2
        (200, 50, 0),   # attack1 - red
        (220, 30, 0),   # attack2
        (200, 200, 0),  # pain - yellow
        (120, 120, 120),# death1 - grey
        (80, 80, 80),   # death2
    ]
    px = [MAGENTA] * (w * h)

    for row in range(8):
        for col in range(8):
            color = state_colors[row]
            cx, cy = col * 64 + 32, row * 64 + 32
            # Draw a simple humanoid: head circle + body rect + arms
            # Head
            for dy in range(-8, -2):
                for dx in range(-5, 6):
                    if dx*dx + dy*dy < 36:
                        px[(cy + dy) * w + (cx + dx)] = color
            # Body
            for dy in range(-2, 16):
                for dx in range(-6, 7):
                    if abs(dx) < 5:
                        px[(cy + dy) * w + (cx + dx)] = color
            # Legs
            for dy in range(16, 26):
                for dx in range(-5, 6):
                    if (dx < -1 or dx > 1):
                        px[(cy + dy) * w + (cx + dx)] = color
            # Arms (vary by rotation for some variety)
            arm_offset = (col - 4) * 1
            for dy in range(0, 10):
                lx = cx - 7 - abs(arm_offset)
                rx = cx + 7 + abs(arm_offset)
                if 0 <= lx < w:
                    px[(cy + dy) * w + lx] = color
                    if lx + 1 < w:
                        px[(cy + dy) * w + lx + 1] = color
                if 0 <= rx < w:
                    px[(cy + dy) * w + rx] = color
                    if rx - 1 >= 0:
                        px[(cy + dy) * w + rx - 1] = color

    return _make_png_rgb(w, h, px)

def gen_rip_pickup(w, h, shape, color):
    """Generate a pickup sprite on magenta background."""
    MAGENTA = (255, 0, 255, 255)
    px = [MAGENTA] * (w * h)
    c = (*color, 255)
    cx, cy = w // 2, h // 2

    if shape == "cross":
        for dy in range(-12, 13):
            for dx in range(-12, 13):
                if abs(dx) < 4 or abs(dy) < 4:
                    y, x = cy + dy, cx + dx
                    if 0 <= x < w and 0 <= y < h:
                        px[y * w + x] = c
    elif shape == "shells":
        for i in range(3):
            ox = cx - 10 + i * 10
            for dy in range(-8, 9):
                for dx in range(-3, 4):
                    y, x = cy + dy, ox + dx
                    if 0 <= x < w and 0 <= y < h:
                        px[y * w + x] = c
    elif shape == "key":
        # Key head
        for dy in range(-10, -2):
            for dx in range(-6, 7):
                if dx*dx + dy*dy < 49:
                    y, x = cy + dy, cx + dx
                    if 0 <= x < w and 0 <= y < h:
                        px[y * w + x] = c
        # Key shaft
        for dy in range(-2, 14):
            for dx in range(-2, 3):
                y, x = cy + dy, cx + dx
                if 0 <= x < w and 0 <= y < h:
                    px[y * w + x] = c
        # Key teeth
        for teeth_y in [6, 10]:
            for dx in range(2, 8):
                y, x = cy + teeth_y, cx + dx
                if 0 <= x < w and 0 <= y < h:
                    px[y * w + x] = c
    elif shape == "barrel":
        for dy in range(-16, 17):
            for dx in range(-10, 11):
                if abs(dx) < 10 - abs(dy) // 4:
                    y, x = cy + dy, cx + dx
                    if 0 <= x < w and 0 <= y < h:
                        px[y * w + x] = c

    return _make_png_rgba(w, h, px)

def gen_rip_weapon(w, h, firing=False):
    """Generate weapon sprite (dark shape at bottom, optional muzzle flash)."""
    MAGENTA = (255, 0, 255, 255)
    px = [MAGENTA] * (w * h)
    cx = w // 2
    dark = (60, 60, 60, 255)
    barrel = (80, 80, 80, 255)

    # Gun barrel (horizontal at bottom half)
    for y in range(h // 2, h - 5):
        for x in range(cx - 15, cx + 16):
            px[y * w + x] = dark
    # Barrel tip
    for y in range(h // 4, h // 2):
        for x in range(cx - 4, cx + 5):
            px[y * w + x] = barrel

    if firing:
        # Muzzle flash
        flash = (255, 220, 50, 255)
        flash2 = (255, 160, 0, 255)
        for y in range(5, h // 4):
            for x in range(cx - 10, cx + 11):
                dy = y - 5
                dx = x - cx
                if dx*dx + dy*dy < (h // 4 - 5)**2:
                    if dx*dx + dy*dy < (h // 8)**2:
                        px[y * w + x] = flash
                    else:
                        px[y * w + x] = flash2

    return _make_png_rgba(w, h, px)

def gen_rip_icon32():
    """32x32 dock icon: crosshair on dark background."""
    w, h = 32, 32
    px = []
    cx, cy = 16, 16
    for y in range(h):
        for x in range(w):
            dx, dy = x - cx, y - cy
            d = math.sqrt(dx*dx + dy*dy)
            if 7 < d < 10:
                px.append((200, 50, 50))
            elif (abs(dx) < 2 and abs(dy) < 12) or (abs(dy) < 2 and abs(dx) < 12):
                px.append((200, 50, 50))
            elif d < 14:
                px.append((30, 30, 40))
            else:
                px.append((20, 20, 30))
    return _make_png_rgb(w, h, px)

def gen_rip_wav(duration_ms, freq, wav_type="sine", sample_rate=11025):
    """Generate a simple WAV file."""
    n_samples = int(sample_rate * duration_ms / 1000)
    import random
    rng = random.Random(freq)  # deterministic per-sound

    pcm = bytearray()
    for i in range(n_samples):
        t = i / sample_rate
        fade = max(0.0, 1.0 - (i / n_samples))
        if wav_type == "sine":
            val = math.sin(2 * math.pi * freq * t) * fade
        elif wav_type == "noise":
            val = (rng.random() * 2 - 1) * fade
        elif wav_type == "sweep_up":
            f = freq + (freq * 2) * (i / n_samples)
            val = math.sin(2 * math.pi * f * t) * fade
        elif wav_type == "sweep_down":
            f = freq * 2 - freq * (i / n_samples)
            val = math.sin(2 * math.pi * f * t) * fade
        elif wav_type == "chime":
            val = (math.sin(2 * math.pi * freq * t) +
                   math.sin(2 * math.pi * freq * 1.5 * t) * 0.5) * fade / 1.5
        else:
            val = 0
        sample = max(-128, min(127, int(val * 127)))
        pcm.append(sample + 128)  # unsigned 8-bit

    data_size = len(pcm)
    hdr = bytearray()
    hdr += b'RIFF'
    hdr += struct.pack('<I', 36 + data_size)
    hdr += b'WAVE'
    hdr += b'fmt '
    hdr += struct.pack('<IHHIIHH', 16, 1, 1, sample_rate, sample_rate, 1, 8)
    hdr += b'data'
    hdr += struct.pack('<I', data_size)
    return bytes(hdr) + bytes(pcm)

def gen_rip_assets():
    """Generate all ChaosRIP game assets. Returns list of (rel_path, data)."""
    assets = []

    # Textures
    assets.append(("data/textures/brick1.png", gen_rip_brick1()))
    assets.append(("data/textures/brick2.png", gen_rip_brick2()))
    assets.append(("data/textures/metal1.png", gen_rip_metal1()))
    assets.append(("data/textures/floor1.png", gen_rip_floor1()))
    assets.append(("data/textures/ceil1.png", gen_rip_ceil1()))
    assets.append(("data/textures/door1.png", gen_rip_door1()))
    assets.append(("data/textures/sky1.png", gen_rip_sky1()))
    assets.append(("data/textures/imp.png", gen_rip_imp_spritesheet()))
    assets.append(("data/textures/health.png", gen_rip_pickup(64, 64, "cross", (50, 200, 50))))
    assets.append(("data/textures/ammo.png", gen_rip_pickup(64, 64, "shells", (200, 200, 50))))
    assets.append(("data/textures/key_red.png", gen_rip_pickup(64, 64, "key", (220, 40, 40))))
    assets.append(("data/textures/key_blue.png", gen_rip_pickup(64, 64, "key", (40, 40, 220))))
    assets.append(("data/textures/barrel.png", gen_rip_pickup(64, 64, "barrel", (50, 150, 50))))
    assets.append(("data/textures/shotgun_idle.png", gen_rip_weapon(80, 80, firing=False)))
    assets.append(("data/textures/shotgun_fire.png", gen_rip_weapon(80, 80, firing=True)))
    assets.append(("data/icon_32.png", gen_rip_icon32()))

    # Sounds
    assets.append(("data/sounds/shotgun.wav", gen_rip_wav(100, 200, "noise")))
    assets.append(("data/sounds/imp_sight.wav", gen_rip_wav(200, 400, "sweep_up")))
    assets.append(("data/sounds/imp_pain.wav", gen_rip_wav(50, 800, "sine")))
    assets.append(("data/sounds/imp_death.wav", gen_rip_wav(300, 300, "sweep_down")))
    assets.append(("data/sounds/pickup.wav", gen_rip_wav(150, 880, "chime")))
    assets.append(("data/sounds/door.wav", gen_rip_wav(400, 80, "noise")))
    assets.append(("data/sounds/pain.wav", gen_rip_wav(100, 150, "noise")))

    return assets


def gen_test_jpeg():
    """Generate a minimal valid JPEG (tiny 2x2 red square).
    This is a hardcoded minimal JFIF that any decoder should handle."""
    # Minimal JPEG: 2x2 red pixels (hand-crafted minimal JFIF)
    # Generated from a known-good minimal JPEG encoder output
    return bytes([
        0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01,
        0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43,
        0x00, 0x08, 0x06, 0x06, 0x07, 0x06, 0x05, 0x08, 0x07, 0x07, 0x07, 0x09,
        0x09, 0x08, 0x0A, 0x0C, 0x14, 0x0D, 0x0C, 0x0B, 0x0B, 0x0C, 0x19, 0x12,
        0x13, 0x0F, 0x14, 0x1D, 0x1A, 0x1F, 0x1E, 0x1D, 0x1A, 0x1C, 0x1C, 0x20,
        0x24, 0x2E, 0x27, 0x20, 0x22, 0x2C, 0x23, 0x1C, 0x1C, 0x28, 0x37, 0x29,
        0x2C, 0x30, 0x31, 0x34, 0x34, 0x34, 0x1F, 0x27, 0x39, 0x3D, 0x38, 0x32,
        0x3C, 0x2E, 0x33, 0x34, 0x32, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x02,
        0x00, 0x02, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xC4, 0x00, 0x1F, 0x00, 0x00,
        0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x10, 0x00, 0x02, 0x01, 0x03,
        0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7D,
        0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
        0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08,
        0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72,
        0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
        0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45,
        0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
        0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75,
        0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
        0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3,
        0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6,
        0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9,
        0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
        0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4,
        0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01,
        0x00, 0x00, 0x3F, 0x00, 0x7B, 0x94, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xD9
    ])

def gen_white_raw():
    return make_raw_header(64, 64) + struct.pack('<I', 0x00FFFFFF) * (64 * 64)

def gen_grid_raw():
    header = make_raw_header(64, 64)
    white = struct.pack('<I', 0x00FFFFFF)
    dark = struct.pack('<I', 0x00404040)
    pixels = bytearray()
    for y in range(64):
        for x in range(64):
            pixels += white if ((x // 8) + (y // 8)) % 2 == 0 else dark
    return header + bytes(pixels)

def gen_flat_normal_raw():
    return make_raw_header(64, 64) + struct.pack('BBBB', 255, 128, 128, 0) * (64 * 64)

def gen_bump_normal_raw():
    header = make_raw_header(64, 64)
    pixels = bytearray()
    cx, cy = 31.5, 31.5
    for y in range(64):
        for x in range(64):
            dx, dy = x - cx, y - cy
            dist = math.sqrt(dx * dx + dy * dy)
            if dist < 0.001:
                nx, ny, nz = 0.0, 0.0, 1.0
            else:
                freq = 0.5
                dh = freq * math.cos(dist * freq)
                gx, gy = dh * dx / dist, dh * dy / dist
                length = math.sqrt(gx * gx + gy * gy + 1.0)
                nx, ny, nz = -gx / length, -gy / length, 1.0 / length
            r = int(max(0, min(255, (nx + 1.0) * 0.5 * 255)))
            g = int(max(0, min(255, (ny + 1.0) * 0.5 * 255)))
            b = int(max(0, min(255, (nz + 1.0) * 0.5 * 255)))
            pixels += struct.pack('BBBB', b, g, r, 0)
    return header + bytes(pixels)

def _pack_cobj(verts, normals, uvs, faces):
    header_size = 40
    vertex_offset = header_size
    normal_offset = vertex_offset + 12 * len(verts)
    uv_offset = normal_offset + 12 * len(normals)
    face_offset = uv_offset + 8 * len(uvs)

    data = bytearray()
    data += struct.pack('<IIIIIIIIII', COBJ_MAGIC, 1, len(verts), len(normals),
                        len(uvs), len(faces), vertex_offset, normal_offset,
                        uv_offset, face_offset)
    for v in verts:
        data += struct.pack('<fff', *v)
    for n in normals:
        data += struct.pack('<fff', *n)
    for uv in uvs:
        data += struct.pack('<ff', *uv)
    for f in faces:
        data += struct.pack('<IIIIIIIII', *f)
    return bytes(data)

def gen_cube_cobj():
    # 8 vertices of a unit cube centered at origin
    verts = [(-0.5,-0.5,-0.5),(0.5,-0.5,-0.5),(0.5,0.5,-0.5),(-0.5,0.5,-0.5),
             (-0.5,-0.5,0.5),(0.5,-0.5,0.5),(0.5,0.5,0.5),(-0.5,0.5,0.5)]
    # 6 face normals (outward-facing)
    normals = [(0,0,-1),(0,0,1),(-1,0,0),(1,0,0),(0,-1,0),(0,1,0)]
    uvs = [(0,0),(1,0),(1,1),(0,1)]
    # Each face: (v0,v1,v2, n0,n1,n2, t0,t1,t2)
    # Winding is CCW when viewed from outside (matches outward normal)
    faces = [
        # Back face (z=-0.5, normal 0,0,-1) — CCW from outside: 0,3,2 and 0,2,1
        (0,3,2, 0,0,0, 0,3,2), (0,2,1, 0,0,0, 0,2,1),
        # Front face (z=+0.5, normal 0,0,1) — CCW from outside: 4,5,6 and 4,6,7
        (4,5,6, 1,1,1, 0,1,2), (4,6,7, 1,1,1, 0,2,3),
        # Left face (x=-0.5, normal -1,0,0) — CCW from outside: 0,4,7 and 0,7,3
        (0,4,7, 2,2,2, 0,1,2), (0,7,3, 2,2,2, 0,2,3),
        # Right face (x=+0.5, normal 1,0,0) — CCW from outside: 1,2,6 and 1,6,5
        (1,2,6, 3,3,3, 0,1,2), (1,6,5, 3,3,3, 0,2,3),
        # Bottom face (y=-0.5, normal 0,-1,0) — CCW from outside: 0,1,5 and 0,5,4
        (0,1,5, 4,4,4, 0,1,2), (0,5,4, 4,4,4, 0,2,3),
        # Top face (y=+0.5, normal 0,1,0) — CCW from outside: 3,7,6 and 3,6,2
        (3,7,6, 5,5,5, 0,1,2), (3,6,2, 5,5,5, 0,2,3),
    ]
    return _pack_cobj(verts, normals, uvs, faces)

def gen_quad_cobj():
    verts = [(-0.5,-0.5,0),(0.5,-0.5,0),(0.5,0.5,0),(-0.5,0.5,0)]
    normals = [(0,0,1)]
    uvs = [(0,0),(1,0),(1,1),(0,1)]
    faces = [(0,1,2,0,0,0,0,1,2),(0,2,3,0,0,0,0,2,3)]
    return _pack_cobj(verts, normals, uvs, faces)

def gen_sphere_cobj():
    lon_segs, lat_segs = 12, 12
    verts, normals, uvs = [], [], []
    for lat in range(lat_segs + 1):
        phi = math.pi * lat / lat_segs
        for lon in range(lon_segs + 1):
            theta = 2.0 * math.pi * lon / lon_segs
            x = math.sin(phi) * math.cos(theta)
            y = math.cos(phi)
            z = math.sin(phi) * math.sin(theta)
            verts.append((x * 0.5, y * 0.5, z * 0.5))
            normals.append((x, y, z))
            uvs.append((lon / lon_segs, lat / lat_segs))

    faces = []
    for lat in range(lat_segs):
        for lon in range(lon_segs):
            i0 = lat * (lon_segs + 1) + lon
            i1, i2, i3 = i0 + 1, i0 + (lon_segs + 1), i0 + (lon_segs + 1) + 1
            if lat != 0:
                faces.append((i0,i1,i3,i0,i1,i3,i0,i1,i3))
            if lat != lat_segs - 1:
                faces.append((i0,i3,i2,i0,i3,i2,i0,i3,i2))
    return _pack_cobj(verts, normals, uvs, faces)

# ═══════════════════════════════════════════════════════════════════════
# Synthetic KAOS test modules (from gen_modules.py)
# ═══════════════════════════════════════════════════════════════════════

def make_corrupt_kaos():
    return bytes([0xDE] * 64)

def make_bad_abi_kaos():
    mod_info = struct.pack('<IIIIIIIIII', 0x4B414F53, 99, 0,0,0,0, 0,0, 0, 0)
    strtab = b'\x00kaos_module_info\x00'
    sym_null = struct.pack('<IIIBBH', 0, 0, 0, 0, 0, 0)
    sym_info = struct.pack('<IIIBBH', 1, 0, 40, 0x11, 0, 1)
    symtab = sym_null + sym_info

    data_off, strtab_off = 52, 52 + 64
    symtab_off = strtab_off + 32
    shdr_off = symtab_off + len(symtab)

    ehdr = bytearray(52)
    ehdr[0:4] = b'\x7fELF'
    ehdr[4], ehdr[5], ehdr[6] = 1, 1, 1
    struct.pack_into('<HH', ehdr, 16, 1, 3)
    struct.pack_into('<I', ehdr, 20, 1)
    struct.pack_into('<I', ehdr, 32, shdr_off)
    struct.pack_into('<H', ehdr, 40, 52)
    struct.pack_into('<H', ehdr, 46, 40)
    struct.pack_into('<H', ehdr, 48, 4)
    struct.pack_into('<H', ehdr, 50, 0)

    shdrs = bytearray(4 * 40)
    struct.pack_into('<IIIIIIIIII', shdrs, 40, 0,1,0x2,0,data_off,len(mod_info),0,0,4,0)
    struct.pack_into('<IIIIIIIIII', shdrs, 80, 0,3,0,0,strtab_off,len(strtab),0,0,1,0)
    struct.pack_into('<IIIIIIIIII', shdrs, 120, 0,2,0,0,symtab_off,len(symtab),2,1,4,16)

    elf = bytearray(ehdr)
    elf += mod_info + b'\x00' * (64 - len(mod_info))
    elf += strtab + b'\x00' * (32 - len(strtab))
    elf += symtab + shdrs
    return bytes(elf)

# ═══════════════════════════════════════════════════════════════════════
# Icon generators (Phase 8) — programmatic RAWT pixel art
# ═══════════════════════════════════════════════════════════════════════

MAGENTA = 0x00FF00FF  # transparency key (BGRX)

def _icon_raw(width, height, draw_fn):
    """Create a RAWT-format .raw file with a draw function for pixel art."""
    pixels = [MAGENTA] * (width * height)
    draw_fn(pixels, width, height)
    data = make_raw_header(width, height)
    for p in pixels:
        data += struct.pack('<I', p)
    return data

def _set_px(pixels, w, x, y, color):
    if 0 <= x < w and 0 <= y < len(pixels) // w:
        pixels[y * w + x] = color

def _fill_rect(pixels, w, x0, y0, rw, rh, color):
    for dy in range(rh):
        for dx in range(rw):
            _set_px(pixels, w, x0 + dx, y0 + dy, color)

def _draw_rect_outline(pixels, w, x0, y0, rw, rh, color):
    for dx in range(rw):
        _set_px(pixels, w, x0 + dx, y0, color)
        _set_px(pixels, w, x0 + dx, y0 + rh - 1, color)
    for dy in range(rh):
        _set_px(pixels, w, x0, y0 + dy, color)
        _set_px(pixels, w, x0 + rw - 1, y0 + dy, color)

def _draw_line(pixels, w, x0, y0, x1, y1, color):
    dx = abs(x1 - x0)
    dy = abs(y1 - y0)
    sx = 1 if x0 < x1 else -1
    sy = 1 if y0 < y1 else -1
    err = dx - dy
    while True:
        _set_px(pixels, w, x0, y0, color)
        if x0 == x1 and y0 == y1:
            break
        e2 = 2 * err
        if e2 > -dy:
            err -= dy
            x0 += sx
        if e2 < dx:
            err += dx
            y0 += sy

# ── Individual icon draw functions ──

def _draw_folder(px, w, h):
    yellow = 0x0000D4FF  # BGRX yellow
    dark = 0x00009BCD
    tab_w = w // 3
    _fill_rect(px, w, 1, h // 5, tab_w, 2, dark)
    _fill_rect(px, w, 1, h // 5 + 1, w - 2, h - h // 5 - 2, yellow)
    _draw_rect_outline(px, w, 1, h // 5 + 1, w - 2, h - h // 5 - 2, dark)

def _draw_file(px, w, h):
    white = 0x00FFFFFF
    gray = 0x00CCCCCC
    fold = max(2, w // 4)
    _fill_rect(px, w, 2, 1, w - 4, h - 2, white)
    _draw_rect_outline(px, w, 2, 1, w - 4, h - 2, gray)
    # Folded corner
    for i in range(fold):
        _draw_line(px, w, w - 2 - fold + i, 1, w - 2 - fold + i, 1 + i, gray)

def _draw_text_file(px, w, h):
    _draw_file(px, w, h)
    line_c = 0x00888888
    for i in range(3):
        y = h // 3 + i * 3
        if y < h - 3:
            _fill_rect(px, w, 5, y, w - 10, 1, line_c)

def _draw_lua(px, w, h):
    blue = 0x00FF4400  # BGRX blue-ish
    cx, cy = w // 2, h // 2
    r = min(w, h) // 2 - 1
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            if dx * dx + dy * dy <= r * r:
                _set_px(px, w, cx + dx, cy + dy, blue)
    # Moon cutout (crescent)
    cr = r * 2 // 3
    ox = r // 2
    for dy in range(-cr, cr + 1):
        for dx in range(-cr, cr + 1):
            if dx * dx + dy * dy <= cr * cr:
                _set_px(px, w, cx + ox + dx, cy + dy - 1, MAGENTA)

def _draw_image(px, w, h):
    sky = 0x00FFCC88  # light blue BGRX
    green = 0x0044AA44
    sun = 0x0000DDFF
    _fill_rect(px, w, 1, 1, w - 2, h - 2, sky)
    _draw_rect_outline(px, w, 1, 1, w - 2, h - 2, 0x00888888)
    # Mountain
    peak_x, peak_y = w // 3, h // 3
    for y_off in range(h - 3 - peak_y):
        lx = max(1, peak_x - y_off)
        rx = min(w - 2, peak_x + y_off)
        _fill_rect(px, w, lx, peak_y + y_off, rx - lx + 1, 1, green)
    # Sun
    sr = max(2, w // 8)
    sx, sy = w * 3 // 4, h // 4
    for dy in range(-sr, sr + 1):
        for dx in range(-sr, sr + 1):
            if dx * dx + dy * dy <= sr * sr:
                _set_px(px, w, sx + dx, sy + dy, sun)

def _draw_audio(px, w, h):
    note_c = 0x00FFFFFF
    # Note head (oval)
    cx, cy = w // 3, h * 2 // 3
    for dy in range(-2, 3):
        for dx in range(-3, 4):
            if dx * dx * 4 + dy * dy * 9 <= 36:
                _set_px(px, w, cx + dx, cy + dy, note_c)
    # Stem
    _fill_rect(px, w, cx + 3, h // 4, 1, cy - h // 4, note_c)
    # Flag
    _draw_line(px, w, cx + 3, h // 4, cx + 7, h // 4 + 4, note_c)

def _draw_binary(px, w, h):
    fg = 0x0044FF44  # green
    text = "01"
    cw = max(1, w // 8)
    for row in range(min(3, h // (cw + 1))):
        for col in range(min(4, w // (cw + 1))):
            c = text[(row + col) % 2]
            x = 1 + col * (cw + 1)
            y = 1 + row * (cw + 2)
            if c == '1':
                _fill_rect(px, w, x, y, cw, cw, fg)
            else:
                _draw_rect_outline(px, w, x, y, cw, cw, fg)

def _draw_cobj(px, w, h):
    c = 0x0088CCFF  # light orange
    m = w // 2
    s = w // 3
    # Simple cube wireframe
    _draw_line(px, w, m - s, m - s // 2, m + s // 2, m - s, c)
    _draw_line(px, w, m + s // 2, m - s, m + s, m - s // 2, c)
    _draw_line(px, w, m - s, m - s // 2, m - s, m + s // 2, c)
    _draw_line(px, w, m - s, m + s // 2, m + s // 2, m + s, c)
    _draw_line(px, w, m + s // 2, m + s, m + s, m + s // 2, c)
    _draw_line(px, w, m + s, m - s // 2, m + s, m + s // 2, c)
    _draw_line(px, w, m + s // 2, m - s, m + s // 2, m + s, c)

def _draw_raw_tex(px, w, h):
    c1 = 0x00FF8800
    c2 = 0x00884400
    cs = max(2, w // 4)
    for y in range(h):
        for x in range(w):
            if ((x // cs) + (y // cs)) % 2 == 0:
                _set_px(px, w, x, y, c1)
            else:
                _set_px(px, w, x, y, c2)

def _draw_shell(px, w, h):
    bg = 0x00333333
    fg = 0x0044FF44
    _fill_rect(px, w, 1, 1, w - 2, h - 2, bg)
    _draw_rect_outline(px, w, 0, 0, w, h, 0x00666666)
    # ">_" prompt
    _draw_line(px, w, w // 5, h // 3, w // 3, h // 2, fg)
    _draw_line(px, w, w // 5, h * 2 // 3, w // 3, h // 2, fg)
    _fill_rect(px, w, w // 3 + 2, h * 2 // 3, w // 3, max(1, h // 16), fg)

def _draw_files_icon(px, w, h):
    _draw_folder(px, w, h)
    # Small magnifying glass overlay
    lens_c = 0x00FFFFFF
    cx, cy = w * 2 // 3, h * 2 // 3
    r = max(2, w // 6)
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            d2 = dx * dx + dy * dy
            if d2 <= r * r and d2 >= (r - 1) * (r - 1):
                _set_px(px, w, cx + dx, cy + dy, lens_c)
    _draw_line(px, w, cx + r, cy + r, cx + r + 2, cy + r + 2, lens_c)

def _draw_settings(px, w, h):
    gray = 0x00AAAAAA
    cx, cy = w // 2, h // 2
    r_outer = min(w, h) // 2 - 1
    r_inner = r_outer * 2 // 3
    r_hole = r_outer // 3
    for dy in range(-r_outer, r_outer + 1):
        for dx in range(-r_outer, r_outer + 1):
            d2 = dx * dx + dy * dy
            if d2 <= r_outer * r_outer and d2 >= r_inner * r_inner:
                _set_px(px, w, cx + dx, cy + dy, gray)
            elif d2 <= r_hole * r_hole:
                _set_px(px, w, cx + dx, cy + dy, gray)
    # Gear teeth (4 cardinal + 4 diagonal)
    tooth = max(2, r_outer // 3)
    for ang in range(0, 360, 45):
        rad = ang * 3.14159 / 180
        tx = int(cx + (r_inner + tooth // 2) * math.cos(rad))
        ty = int(cy + (r_inner + tooth // 2) * math.sin(rad))
        _fill_rect(px, w, tx - 1, ty - 1, 3, 3, gray)

def _draw_claude(px, w, h):
    orange = 0x00FF8800  # BGRX accent
    cx, cy = w // 2, h // 2
    # Star / spark shape
    r = min(w, h) // 2 - 2
    for i in range(4):
        ang = i * 3.14159 / 2
        for t in range(r):
            x = int(cx + t * math.cos(ang))
            y = int(cy + t * math.sin(ang))
            _set_px(px, w, x, y, orange)
            if t > 0:
                # Perpendicular width
                px2 = int(cx + t * math.cos(ang) + math.sin(ang))
                py2 = int(cy + t * math.sin(ang) - math.cos(ang))
                _set_px(px, w, px2, py2, orange)

def _draw_close(px, w, h):
    c = 0x00FFFFFF
    _draw_line(px, w, 3, 3, w - 4, h - 4, c)
    _draw_line(px, w, w - 4, 3, 3, h - 4, c)

def _draw_minimize(px, w, h):
    c = 0x00FFFFFF
    _fill_rect(px, w, 3, h // 2, w - 6, 2, c)

def _draw_maximize(px, w, h):
    c = 0x00FFFFFF
    _draw_rect_outline(px, w, 3, 3, w - 6, h - 6, c)

def _draw_cursor(px, w, h):
    white = 0x00FFFFFF
    black = 0x00000000
    # Arrow pointer — white filled with black outline
    # Approximately 20px tall arrow pointing upper-left
    arrow = [
        (0, 0), (0, 1), (0, 2), (0, 3), (0, 4), (0, 5), (0, 6), (0, 7),
        (0, 8), (0, 9), (0, 10), (0, 11), (0, 12), (0, 13), (0, 14), (0, 15),
        (0, 16), (0, 17), (0, 18),
        (1, 1), (1, 2), (1, 3), (1, 4), (1, 5), (1, 6), (1, 7), (1, 8),
        (1, 9), (1, 10), (1, 11), (1, 12), (1, 13), (1, 14), (1, 15),
        (2, 2), (2, 3), (2, 4), (2, 5), (2, 6), (2, 7), (2, 8),
        (2, 9), (2, 10), (2, 11), (2, 12), (2, 13), (2, 14),
        (3, 3), (3, 4), (3, 5), (3, 6), (3, 7), (3, 8), (3, 9),
        (3, 10), (3, 11), (3, 12), (3, 13),
        (4, 4), (4, 5), (4, 6), (4, 7), (4, 8), (4, 9), (4, 10), (4, 11), (4, 12),
        (5, 5), (5, 6), (5, 7), (5, 8), (5, 9), (5, 10), (5, 11),
        (6, 6), (6, 7), (6, 8), (6, 9), (6, 10),
        (7, 7), (7, 8), (7, 9),
        (8, 8),
    ]
    # Draw black outline first
    for x, y in arrow:
        for dx in [-1, 0, 1]:
            for dy in [-1, 0, 1]:
                _set_px(px, w, x + dx, y + dy, black)
    # Draw white fill
    for x, y in arrow:
        _set_px(px, w, x, y, white)

def _draw_mod_default(px, w, h):
    c = 0x00AAAAAA
    # Puzzle piece shape
    cx, cy = w // 2, h // 2
    # Main body
    _fill_rect(px, w, w // 4, h // 4, w // 2, h // 2, c)
    # Top tab
    _fill_rect(px, w, cx - 2, h // 4 - 3, 4, 4, c)
    # Right tab
    _fill_rect(px, w, w * 3 // 4, cy - 2, 4, 4, c)
    # Bottom notch
    _fill_rect(px, w, cx - 2, h * 3 // 4, 4, 3, 0x00666666)
    # Left notch
    _fill_rect(px, w, w // 4 - 3, cy - 2, 3, 4, 0x00666666)

def _draw_missing(px, w, h):
    magenta_solid = 0x00FF00FF
    fg = 0x00FFFFFF
    _fill_rect(px, w, 0, 0, w, h, magenta_solid)
    # Draw "?" character
    cx = w // 2
    _fill_rect(px, w, cx - 2, 2, 4, 2, fg)
    _fill_rect(px, w, cx + 1, 4, 2, 2, fg)
    _fill_rect(px, w, cx, 6, 2, 2, fg)
    _fill_rect(px, w, cx, h - 5, 2, 2, fg)


def gen_all_icons():
    """Generate all icon .raw files. Returns list of (path, data) tuples."""
    icons = []

    icon_defs = [
        ("folder",   [16, 32, 48], _draw_folder),
        ("file",     [16, 32, 48], _draw_file),
        ("text",     [16, 32, 48], _draw_text_file),
        ("lua",      [16, 32],     _draw_lua),
        ("image",    [16, 32],     _draw_image),
        ("audio",    [16, 32],     _draw_audio),
        ("binary",   [16, 32],     _draw_binary),
        ("cobj",     [32],         _draw_cobj),
        ("raw",      [32],         _draw_raw_tex),
        ("shell",    [32, 48],     _draw_shell),
        ("files",    [32, 48],     _draw_files_icon),
        ("settings", [32, 48],     _draw_settings),
        ("claude",   [32, 48],     _draw_claude),
        ("close",    [16],         _draw_close),
        ("minimize", [16],         _draw_minimize),
        ("maximize", [16],         _draw_maximize),
        ("missing",  [16, 32],     _draw_missing),
        ("cursor",   [24],         _draw_cursor),
        ("mod_default", [32],      _draw_mod_default),
    ]

    for name, sizes, draw_fn in icon_defs:
        for sz in sizes:
            data = _icon_raw(sz, sz, draw_fn)
            path = f"{name}_{sz}.raw"
            icons.append((path, data))

    return icons


# ═══════════════════════════════════════════════════════════════════════
# Main: format + populate
# ═══════════════════════════════════════════════════════════════════════

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <disk_image> <lba_start> [modules_dir]", file=sys.stderr)
        sys.exit(1)

    image_path = sys.argv[1]
    lba_start = int(sys.argv[2])
    modules_dir = sys.argv[3] if len(sys.argv) > 3 else None

    # Find project root (parent of tools/)
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(script_dir)
    harddrive_dir = os.path.join(project_root, 'harddrive')

    with open(image_path, 'r+b') as f:
        # ── Step 1: Format ──────────────────────────────
        format_chaosfs(f, lba_start)

        # ── Step 2: Create writer for populating ────────
        writer = ChaosFSWriter(f, lba_start)

        # ── Step 3: Copy harddrive/ contents ────────────
        if os.path.isdir(harddrive_dir):
            print(f"populate_fs: copying harddrive/ contents")
            _copy_directory_tree(writer, harddrive_dir, ROOT_INO)
        else:
            print(f"populate_fs: no harddrive/ directory found, skipping")

        # ── Step 4: Test assets (/test/) ────────────────
        test_assets = [
            ("white.raw",       gen_white_raw),
            ("grid.raw",        gen_grid_raw),
            ("flat_normal.raw", gen_flat_normal_raw),
            ("bump_normal.raw", gen_bump_normal_raw),
            ("cube.cobj",       gen_cube_cobj),
            ("quad.cobj",       gen_quad_cobj),
            ("sphere.cobj",     gen_sphere_cobj),
        ]
        test_dir_ino = writer.ensure_directory("/test")
        for name, gen_fn in test_assets:
            data = gen_fn()
            writer.create_file(test_dir_ino, name, data)
            print(f"populate_fs: wrote /test/{name} ({len(data)} bytes)")

        # ── Step 4b: Audio test files (/sounds/) ─────────
        sounds_ino = writer.ensure_directory("/sounds")

        # Generate test.wav: 1 second 440Hz sine wave, 48kHz stereo 16-bit
        import struct as struct_mod, math as math_mod
        wav_rate = 48000
        wav_duration = 1.0
        wav_samples = int(wav_rate * wav_duration)
        wav_pcm = bytearray()
        for i in range(wav_samples):
            val = int(32767 * math_mod.sin(2 * math_mod.pi * 440 * i / wav_rate))
            wav_pcm += struct_mod.pack('<hh', val, val)
        wav_data_size = len(wav_pcm)
        wav_header = bytearray()
        wav_header += b'RIFF'
        wav_header += struct_mod.pack('<I', 36 + wav_data_size)
        wav_header += b'WAVE'
        wav_header += b'fmt '
        wav_header += struct_mod.pack('<IHHIIHH', 16, 1, 2, wav_rate, wav_rate * 4, 4, 16)
        wav_header += b'data'
        wav_header += struct_mod.pack('<I', wav_data_size)
        test_wav = bytes(wav_header) + bytes(wav_pcm)
        writer.create_file(sounds_ino, "test.wav", test_wav)
        print(f"populate_fs: wrote /sounds/test.wav ({len(test_wav)} bytes)")

        # Generate chime.wav: 0.25 second 880Hz
        chime_samples = int(wav_rate * 0.25)
        chime_pcm = bytearray()
        for i in range(chime_samples):
            fade = 1.0 - (i / chime_samples)
            val = int(32767 * fade * math_mod.sin(2 * math_mod.pi * 880 * i / wav_rate))
            chime_pcm += struct_mod.pack('<hh', val, val)
        chime_data_size = len(chime_pcm)
        chime_header = bytearray()
        chime_header += b'RIFF'
        chime_header += struct_mod.pack('<I', 36 + chime_data_size)
        chime_header += b'WAVE'
        chime_header += b'fmt '
        chime_header += struct_mod.pack('<IHHIIHH', 16, 1, 2, wav_rate, wav_rate * 4, 4, 16)
        chime_header += b'data'
        chime_header += struct_mod.pack('<I', chime_data_size)
        chime_wav = bytes(chime_header) + bytes(chime_pcm)
        writer.create_file(sounds_ino, "chime.wav", chime_wav)
        print(f"populate_fs: wrote /sounds/chime.wav ({len(chime_wav)} bytes)")

        # Generate test.mid: C major scale (C4-C5)
        def make_midi_scale():
            """Generate a minimal MIDI file with a C major scale."""
            import struct as s
            # MThd
            hdr = b'MThd' + s.pack('>IHhH', 6, 0, 1, 480)  # Format 0, 1 track, 480 tpq
            # MTrk
            events = bytearray()
            # Tempo: 120 BPM = 500000 usec/quarter
            events += b'\x00\xff\x51\x03' + s.pack('>I', 500000)[1:]
            # Program change: piano (channel 0, program 0)
            events += b'\x00\xc0\x00'
            # Notes: C4=60, D4=62, E4=64, F4=65, G4=67, A4=69, B4=71, C5=72
            notes = [60, 62, 64, 65, 67, 69, 71, 72]
            for note in notes:
                events += b'\x00' + bytes([0x90, note, 100])  # note on, vel=100
                # 480 ticks = 1 quarter note at 120BPM = 0.5s
                events += b'\x83\x60' + bytes([0x80, note, 0])  # delta=480, note off
            # End of track
            events += b'\x00\xff\x2f\x00'
            trk = b'MTrk' + s.pack('>I', len(events)) + bytes(events)
            return hdr + trk

        test_mid = make_midi_scale()
        writer.create_file(sounds_ino, "test.mid", test_mid)
        print(f"populate_fs: wrote /sounds/test.mid ({len(test_mid)} bytes)")

        # ── Step 4c: Image test files (/test/) ───────────
        test_png = gen_test_png()
        writer.create_file(test_dir_ino, "alpha_test.png", test_png)
        print(f"populate_fs: wrote /test/alpha_test.png ({len(test_png)} bytes)")

        # ── Step 5: Compiled .kaos modules ──────────────
        modules_ino = writer.ensure_directory("/system/modules")

        if modules_dir and os.path.isdir(modules_dir):
            kaos_files = sorted(fn for fn in os.listdir(modules_dir) if fn.endswith('.kaos'))
            for fn in kaos_files:
                filepath = os.path.join(modules_dir, fn)
                with open(filepath, 'rb') as mf:
                    data = mf.read()
                writer.create_file(modules_ino, fn, data)
                print(f"populate_fs: wrote /system/modules/{fn} ({len(data)} bytes)")

        # ── Step 5a: ChaosRIP game assets ─────────────────
        rip_assets = gen_rip_assets()
        for rel_path, data in rip_assets:
            full_path = f"/apps/rip/{rel_path}"
            # Ensure parent directory exists
            parent_path = '/'.join(full_path.split('/')[:-1])
            parent_ino = writer.ensure_directory(parent_path)
            filename = full_path.split('/')[-1]
            writer.create_file(parent_ino, filename, data)
            print(f"populate_fs: wrote {full_path} ({len(data)} bytes)")
        print(f"populate_fs: generated {len(rip_assets)} ChaosRIP assets")

        # ── Step 5b: Generate icons (/system/icons/) ─────
        icons_ino = writer.ensure_directory("/system/icons")
        icon_files = gen_all_icons()
        for name, data in icon_files:
            writer.create_file(icons_ino, name, data)
        print(f"populate_fs: wrote {len(icon_files)} icon files to /system/icons/")

        # ── Step 5c: Desktop shortcut files (/desktop/) ──
        desktop_ino = writer.ensure_directory("/desktop")
        desk_shortcuts = [
            ("Files.desk",    'return { app = "/apps/files/main.lua", icon = "/system/icons/files_48.png" }\n'),
            ("Terminal.desk", 'return { app = "/apps/terminal/main.lua", icon = "/system/icons/terminal_48.png" }\n'),
            ("Settings.desk", 'return { app = "/apps/settings/main.lua", icon = "/system/icons/settings_48.png" }\n'),
            ("Editor.desk",   'return { app = "/apps/edit/main.lua", icon = "/system/icons/edit_48.png" }\n'),
        ]
        for name, content in desk_shortcuts:
            writer.create_file(desktop_ino, name, content.encode('utf-8'))
            print(f"populate_fs: wrote /desktop/{name}")

        # Create /system/trash/ directory
        writer.ensure_directory("/system/trash")

        # Test modules only needed for phase tests (disabled in interactive boot)
        # for name, gen_fn in [("corrupt.kaos", make_corrupt_kaos), ("bad_abi.kaos", make_bad_abi_kaos)]:
        #     data = gen_fn()
        #     writer.create_file(modules_ino, name, data)

        # ── Step 6: Flush ───────────────────────────────
        writer.flush()

    print(f"populate_fs: done ({writer.files_written} files, {writer.dirs_created} dirs, "
          f"{writer.blocks_allocated} blocks, {writer.inodes_allocated} inodes)")


def _copy_directory_tree(writer, host_dir, parent_ino, fs_path=""):
    """Recursively copy a host directory into ChaosFS."""
    entries = sorted(os.listdir(host_dir))
    for entry in entries:
        host_path = os.path.join(host_dir, entry)
        if entry.startswith('.'):
            continue  # skip hidden files

        entry_fs_path = f"{fs_path}/{entry}"
        if os.path.isdir(host_path):
            dir_ino = writer.create_directory(parent_ino, entry)
            print(f"populate_fs: created dir {entry_fs_path}/")
            _copy_directory_tree(writer, host_path, dir_ino, entry_fs_path)
        elif os.path.isfile(host_path):
            with open(host_path, 'rb') as f:
                data = f.read()
            writer.create_file(parent_ino, entry, data)
            print(f"populate_fs: wrote {entry_fs_path} ({len(data)} bytes)")


if __name__ == '__main__':
    main()
