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
    verts = [(-0.5,-0.5,-0.5),(0.5,-0.5,-0.5),(0.5,0.5,-0.5),(-0.5,0.5,-0.5),
             (-0.5,-0.5,0.5),(0.5,-0.5,0.5),(0.5,0.5,0.5),(-0.5,0.5,0.5)]
    normals = [(0,0,-1),(0,0,1),(-1,0,0),(1,0,0),(0,-1,0),(0,1,0)]
    uvs = [(0,0),(1,0),(1,1),(0,1)]
    faces = [
        (0,1,2,0,0,0,0,1,2),(0,2,3,0,0,0,0,2,3),(5,4,7,1,1,1,0,1,2),(5,7,6,1,1,1,0,2,3),
        (4,0,3,2,2,2,0,1,2),(4,3,7,2,2,2,0,2,3),(1,5,6,3,3,3,0,1,2),(1,6,2,3,3,3,0,2,3),
        (4,5,1,4,4,4,0,1,2),(4,1,0,4,4,4,0,2,3),(3,2,6,5,5,5,0,1,2),(3,6,7,5,5,5,0,2,3),
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
        ("text",     [16, 32],     _draw_text_file),
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

        # ── Step 5b: Generate icons (/system/icons/) ─────
        icons_ino = writer.ensure_directory("/system/icons")
        icon_files = gen_all_icons()
        for name, data in icon_files:
            writer.create_file(icons_ino, name, data)
        print(f"populate_fs: wrote {len(icon_files)} icon files to /system/icons/")

        # Synthetic test modules
        for name, gen_fn in [("corrupt.kaos", make_corrupt_kaos), ("bad_abi.kaos", make_bad_abi_kaos)]:
            data = gen_fn()
            writer.create_file(modules_ino, name, data)
            print(f"populate_fs: wrote /system/modules/{name} ({len(data)} bytes)")

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
