#!/usr/bin/env python3
"""gen_assets — Generate ChaosGL test assets and write them into a ChaosFS disk image.
Usage: python gen_assets.py <disk_image> <lba_start>
Run AFTER mkfs_chaos.py has formatted the filesystem."""

import struct
import sys
import math

# Constants (must match mkfs_chaos.py / chaos_types.h)
CHAOS_MAGIC = 0x43484653
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

# .raw texture header magic
RAW_MAGIC = 0x52415754  # 'RAWT'

# .cobj model magic
COBJ_MAGIC = 0x434F424A


# ---------------------------------------------------------------------------
# Disk I/O helpers
# ---------------------------------------------------------------------------

def block_offset(lba_start, block_idx):
    """Byte offset in disk image for a given ChaosFS block."""
    return (lba_start + block_idx * CHAOS_SECTORS_PER_BLK) * 512


def read_block(f, lba_start, block_idx):
    f.seek(block_offset(lba_start, block_idx))
    return bytearray(f.read(CHAOS_BLOCK_SIZE))


def write_block(f, lba_start, block_idx, data):
    assert len(data) == CHAOS_BLOCK_SIZE
    f.seek(block_offset(lba_start, block_idx))
    f.write(data)


def crc32_chaos(data):
    """CRC-32/ISO-HDLC, polynomial 0xEDB88320."""
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1))
    return crc ^ 0xFFFFFFFF


# ---------------------------------------------------------------------------
# Superblock parsing / writing
# ---------------------------------------------------------------------------

class Superblock:
    """Minimal parsed superblock."""
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
        """Recompute and store checksum."""
        # Checksum offset: 4+4+16+4+4+4+4+4+4+4+4+4+1+1+2+4+4+4+4+4+28 = 116
        checksum_offset = 116
        crc = crc32_chaos(self.raw[:checksum_offset])
        struct.pack_into('<I', self.raw, checksum_offset, crc)

    def to_bytes(self):
        self.recompute_crc()
        return bytes(self.raw)


# ---------------------------------------------------------------------------
# Inode helpers
# ---------------------------------------------------------------------------

def make_inode(mode, link_count, size, extent_start, extent_blocks):
    """Build a 128-byte inode (matches mkfs_chaos.py layout)."""
    ino = bytearray(CHAOS_INODE_SIZE)
    struct.pack_into('<H', ino, 0, CHAOS_INODE_MAGIC)
    struct.pack_into('<H', ino, 2, mode)
    struct.pack_into('<I', ino, 4, link_count)
    struct.pack_into('<Q', ino, 16, size)
    struct.pack_into('<B', ino, 40, 1)  # extent_count
    struct.pack_into('<I', ino, 44, extent_start)
    struct.pack_into('<I', ino, 48, extent_blocks)
    return bytes(ino)


def make_dirent(inode_num, dtype, name):
    """Build a 64-byte directory entry."""
    d = bytearray(CHAOS_DIRENT_SIZE)
    name_bytes = name.encode('ascii')
    struct.pack_into('<I', d, 0, inode_num)
    struct.pack_into('<B', d, 4, dtype)
    struct.pack_into('<B', d, 5, len(name_bytes))
    d[6:6 + len(name_bytes)] = name_bytes
    return bytes(d)


# ---------------------------------------------------------------------------
# Bitmap helpers
# ---------------------------------------------------------------------------

class Bitmap:
    def __init__(self, data):
        self.data = bytearray(data)

    def is_used(self, bit):
        return bool(self.data[bit // 8] & (1 << (bit % 8)))

    def set_used(self, bit):
        self.data[bit // 8] |= (1 << (bit % 8))

    def alloc_contiguous(self, count, search_start):
        """Find and allocate `count` contiguous free blocks starting from search_start."""
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


# ---------------------------------------------------------------------------
# ChaosFS writer context
# ---------------------------------------------------------------------------

class ChaosFSWriter:
    def __init__(self, f, lba_start):
        self.f = f
        self.lba_start = lba_start

        # Read superblock
        sb_raw = read_block(f, lba_start, 0)
        self.sb = Superblock(sb_raw)
        assert self.sb.magic == CHAOS_MAGIC, f"Bad magic: 0x{self.sb.magic:08X}"

        # Read bitmap
        bmp_data = bytearray()
        for i in range(self.sb.bitmap_blocks):
            bmp_data += read_block(f, lba_start, self.sb.bitmap_start + i)
        self.bitmap = Bitmap(bmp_data)

        # Track next free inode search position
        self.next_inode_search = 2  # 0 is unused, 1 is root
        self.blocks_allocated = 0
        self.inodes_allocated = 0

    def alloc_inode(self):
        """Find a free inode slot (magic == 0 means free). Returns inode number."""
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
        """Write a 128-byte inode to its slot on disk."""
        tbl_block = self.sb.inode_table_start + (ino_num // CHAOS_INODES_PER_BLOCK)
        slot = ino_num % CHAOS_INODES_PER_BLOCK
        blk = read_block(self.f, self.lba_start, tbl_block)
        off = slot * CHAOS_INODE_SIZE
        blk[off:off + CHAOS_INODE_SIZE] = inode_data
        write_block(self.f, self.lba_start, tbl_block, bytes(blk))

    def alloc_blocks(self, count):
        """Allocate contiguous data blocks. Returns start block index."""
        start = self.bitmap.alloc_contiguous(count, self.sb.data_start)
        self.blocks_allocated += count
        return start

    def write_file_data(self, start_block, data):
        """Write arbitrary data to allocated blocks (pads last block with zeros)."""
        num_blocks = (len(data) + CHAOS_BLOCK_SIZE - 1) // CHAOS_BLOCK_SIZE
        padded = bytearray(data)
        padded += b'\x00' * (num_blocks * CHAOS_BLOCK_SIZE - len(data))
        for i in range(num_blocks):
            chunk = padded[i * CHAOS_BLOCK_SIZE:(i + 1) * CHAOS_BLOCK_SIZE]
            write_block(self.f, self.lba_start, start_block + i, bytes(chunk))

    def add_dirent_to_dir(self, dir_ino_num, child_ino, dtype, name):
        """Append a directory entry to an existing directory's data block."""
        # Read the directory inode to find its data block
        tbl_block = self.sb.inode_table_start + (dir_ino_num // CHAOS_INODES_PER_BLOCK)
        slot = dir_ino_num % CHAOS_INODES_PER_BLOCK
        ino_blk = read_block(self.f, self.lba_start, tbl_block)
        off = slot * CHAOS_INODE_SIZE
        (extent_start,) = struct.unpack_from('<I', ino_blk, off + 44)

        # Read the directory data block
        dir_data = read_block(self.f, self.lba_start, extent_start)

        # Scan for first empty dirent slot (inode_num == 0 means free)
        new_off = -1
        max_entries = CHAOS_BLOCK_SIZE // CHAOS_DIRENT_SIZE
        for i in range(max_entries):
            entry_off = i * CHAOS_DIRENT_SIZE
            (ino,) = struct.unpack_from('<I', dir_data, entry_off)
            if ino == 0:
                new_off = entry_off
                break

        if new_off < 0:
            raise RuntimeError(f"Directory inode {dir_ino_num} is full (single block)")

        # Write new entry
        entry = make_dirent(child_ino, dtype, name)
        dir_data[new_off:new_off + CHAOS_DIRENT_SIZE] = entry
        write_block(self.f, self.lba_start, extent_start, bytes(dir_data))

    def create_directory(self, parent_ino, name):
        """Create a subdirectory under parent_ino. Returns new inode number."""
        ino_num = self.alloc_inode()
        data_block = self.alloc_blocks(1)

        # Write directory data: "." and ".."
        dir_data = bytearray(CHAOS_BLOCK_SIZE)
        dir_data[0:CHAOS_DIRENT_SIZE] = make_dirent(ino_num, CHAOS_DT_DIR, ".")
        dir_data[CHAOS_DIRENT_SIZE:CHAOS_DIRENT_SIZE * 2] = make_dirent(parent_ino, CHAOS_DT_DIR, "..")
        write_block(self.f, self.lba_start, data_block, bytes(dir_data))

        # Write inode
        dir_size = CHAOS_DIRENT_SIZE * 2  # "." and ".."
        inode = make_inode(CHAOS_TYPE_DIR | 0o755, 2, dir_size, data_block, 1)
        self.write_inode(ino_num, inode)

        # Add entry in parent
        self.add_dirent_to_dir(parent_ino, ino_num, CHAOS_DT_DIR, name)

        return ino_num

    def create_file(self, parent_ino, name, data):
        """Create a regular file under parent_ino. Returns inode number."""
        ino_num = self.alloc_inode()
        num_blocks = max(1, (len(data) + CHAOS_BLOCK_SIZE - 1) // CHAOS_BLOCK_SIZE)
        start_block = self.alloc_blocks(num_blocks)

        # Write data
        self.write_file_data(start_block, data)

        # Write inode
        inode = make_inode(CHAOS_TYPE_FILE | 0o644, 1, len(data), start_block, num_blocks)
        self.write_inode(ino_num, inode)

        # Add directory entry
        self.add_dirent_to_dir(parent_ino, ino_num, CHAOS_DT_REG, name)

        return ino_num

    def flush(self):
        """Write updated bitmap and superblock back to disk."""
        # Write bitmap
        for i in range(self.sb.bitmap_blocks):
            chunk = self.bitmap.data[i * CHAOS_BLOCK_SIZE:(i + 1) * CHAOS_BLOCK_SIZE]
            write_block(self.f, self.lba_start, self.sb.bitmap_start + i, bytes(chunk))

        # Update superblock counters
        self.sb.set_free_blocks(self.sb.free_blocks - self.blocks_allocated)
        self.sb.set_free_inodes(self.sb.free_inodes - self.inodes_allocated)

        # Write superblock (primary + backup)
        sb_bytes = self.sb.to_bytes()
        write_block(self.f, self.lba_start, 0, sb_bytes)
        write_block(self.f, self.lba_start, 1, sb_bytes)


# ---------------------------------------------------------------------------
# Asset generators
# ---------------------------------------------------------------------------

def make_raw_header(width, height):
    """16-byte .raw texture header."""
    return struct.pack('<IIII', RAW_MAGIC, width, height, 0)


def gen_white_raw():
    """64x64 solid white BGRX texture."""
    header = make_raw_header(64, 64)
    # BGRX: B=0xFF, G=0xFF, R=0xFF, X=0x00 => 0x00FFFFFF in little-endian as FF FF FF 00
    pixel = struct.pack('<I', 0x00FFFFFF)
    pixels = pixel * (64 * 64)
    return header + pixels


def gen_grid_raw():
    """64x64 checkerboard grid BGRX texture."""
    header = make_raw_header(64, 64)
    white = struct.pack('<I', 0x00FFFFFF)
    dark = struct.pack('<I', 0x00404040)
    pixels = bytearray()
    for y in range(64):
        for x in range(64):
            if ((x // 8) + (y // 8)) % 2 == 0:
                pixels += white
            else:
                pixels += dark
    return header + bytes(pixels)


def gen_flat_normal_raw():
    """64x64 flat normal map. Normal (0,0,1) encoded as R=128,G=128,B=255."""
    header = make_raw_header(64, 64)
    # BGRX order: B=255, G=128, R=128, X=0
    pixel = struct.pack('BBBB', 255, 128, 128, 0)
    pixels = pixel * (64 * 64)
    return header + pixels


def gen_bump_normal_raw():
    """64x64 bumpy normal map with concentric ring pattern."""
    header = make_raw_header(64, 64)
    pixels = bytearray()
    cx, cy = 31.5, 31.5
    for y in range(64):
        for x in range(64):
            dx = x - cx
            dy = y - cy
            dist = math.sqrt(dx * dx + dy * dy)
            if dist < 0.001:
                nx, ny, nz = 0.0, 0.0, 1.0
            else:
                # Sine-wave bump: height = sin(dist * freq)
                freq = 0.5
                dh = freq * math.cos(dist * freq)  # derivative of height
                # Normal from height field: (-dh_dx, -dh_dy, 1) normalized
                # dh_dx = dh * dx/dist, dh_dy = dh * dy/dist
                gx = dh * dx / dist
                gy = dh * dy / dist
                length = math.sqrt(gx * gx + gy * gy + 1.0)
                nx = -gx / length
                ny = -gy / length
                nz = 1.0 / length
            # Encode: component = (n + 1) / 2 * 255
            r = int(max(0, min(255, (nx + 1.0) * 0.5 * 255)))
            g = int(max(0, min(255, (ny + 1.0) * 0.5 * 255)))
            b = int(max(0, min(255, (nz + 1.0) * 0.5 * 255)))
            # BGRX
            pixels += struct.pack('BBBB', b, g, r, 0)
    return header + bytes(pixels)


def gen_cube_cobj():
    """Unit cube: 8 vertices, 6 normals, 4 UVs, 12 triangular faces."""
    # 8 cube vertices at +/-0.5
    verts = [
        (-0.5, -0.5, -0.5),  # 0
        ( 0.5, -0.5, -0.5),  # 1
        ( 0.5,  0.5, -0.5),  # 2
        (-0.5,  0.5, -0.5),  # 3
        (-0.5, -0.5,  0.5),  # 4
        ( 0.5, -0.5,  0.5),  # 5
        ( 0.5,  0.5,  0.5),  # 6
        (-0.5,  0.5,  0.5),  # 7
    ]

    # 6 face normals
    normals = [
        ( 0.0,  0.0, -1.0),  # 0: front  (z-)
        ( 0.0,  0.0,  1.0),  # 1: back   (z+)
        (-1.0,  0.0,  0.0),  # 2: left   (x-)
        ( 1.0,  0.0,  0.0),  # 3: right  (x+)
        ( 0.0, -1.0,  0.0),  # 4: bottom (y-)
        ( 0.0,  1.0,  0.0),  # 5: top    (y+)
    ]

    # 4 UVs
    uvs = [
        (0.0, 0.0),
        (1.0, 0.0),
        (1.0, 1.0),
        (0.0, 1.0),
    ]

    # 12 faces (v[3], n[3], t[3]) — 2 triangles per cube face
    # Each face: quad split into 2 tris, all sharing same normal index
    faces = [
        # Front face (z-): verts 0,1,2,3, normal 0
        (0, 1, 2, 0, 0, 0, 0, 1, 2),
        (0, 2, 3, 0, 0, 0, 0, 2, 3),
        # Back face (z+): verts 5,4,7,6, normal 1
        (5, 4, 7, 1, 1, 1, 0, 1, 2),
        (5, 7, 6, 1, 1, 1, 0, 2, 3),
        # Left face (x-): verts 4,0,3,7, normal 2
        (4, 0, 3, 2, 2, 2, 0, 1, 2),
        (4, 3, 7, 2, 2, 2, 0, 2, 3),
        # Right face (x+): verts 1,5,6,2, normal 3
        (1, 5, 6, 3, 3, 3, 0, 1, 2),
        (1, 6, 2, 3, 3, 3, 0, 2, 3),
        # Bottom face (y-): verts 4,5,1,0, normal 4
        (4, 5, 1, 4, 4, 4, 0, 1, 2),
        (4, 1, 0, 4, 4, 4, 0, 2, 3),
        # Top face (y+): verts 3,2,6,7, normal 5
        (3, 2, 6, 5, 5, 5, 0, 1, 2),
        (3, 6, 7, 5, 5, 5, 0, 2, 3),
    ]

    return _pack_cobj(verts, normals, uvs, faces)


def gen_quad_cobj():
    """Flat quad: 4 vertices, 1 normal, 4 UVs, 2 faces."""
    verts = [
        (-0.5, -0.5, 0.0),
        ( 0.5, -0.5, 0.0),
        ( 0.5,  0.5, 0.0),
        (-0.5,  0.5, 0.0),
    ]
    normals = [(0.0, 0.0, 1.0)]
    uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    faces = [
        (0, 1, 2, 0, 0, 0, 0, 1, 2),
        (0, 2, 3, 0, 0, 0, 0, 2, 3),
    ]
    return _pack_cobj(verts, normals, uvs, faces)


def gen_sphere_cobj():
    """UV sphere with 12 longitude x 12 latitude segments."""
    lon_segs = 12
    lat_segs = 12

    verts = []
    normals = []
    uvs = []

    # Generate grid vertices/normals/UVs
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
            # Indices into the grid
            i0 = lat * (lon_segs + 1) + lon
            i1 = i0 + 1
            i2 = i0 + (lon_segs + 1)
            i3 = i2 + 1

            # Skip degenerate triangles at poles
            if lat != 0:
                faces.append((i0, i1, i3, i0, i1, i3, i0, i1, i3))
            if lat != lat_segs - 1:
                faces.append((i0, i3, i2, i0, i3, i2, i0, i3, i2))

    return _pack_cobj(verts, normals, uvs, faces)


def _pack_cobj(verts, normals, uvs, faces):
    """Pack vertices/normals/uvs/faces into binary .cobj format."""
    header_size = 40
    vert_size = 12 * len(verts)
    norm_size = 12 * len(normals)
    uv_size = 8 * len(uvs)
    face_size = 36 * len(faces)

    vertex_offset = header_size
    normal_offset = vertex_offset + vert_size
    uv_offset = normal_offset + norm_size
    face_offset = uv_offset + uv_size

    data = bytearray()

    # Header (40 bytes)
    data += struct.pack('<IIIIIIIIII',
                        COBJ_MAGIC,
                        1,  # version
                        len(verts),
                        len(normals),
                        len(uvs),
                        len(faces),
                        vertex_offset,
                        normal_offset,
                        uv_offset,
                        face_offset)

    # Vertices
    for v in verts:
        data += struct.pack('<fff', v[0], v[1], v[2])

    # Normals
    for n in normals:
        data += struct.pack('<fff', n[0], n[1], n[2])

    # UVs
    for uv in uvs:
        data += struct.pack('<ff', uv[0], uv[1])

    # Faces: v[3], n[3], t[3] — 9 uint32s = 36 bytes each
    for f in faces:
        data += struct.pack('<IIIIIIIII', f[0], f[1], f[2],
                            f[3], f[4], f[5],
                            f[6], f[7], f[8])

    return bytes(data)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <disk_image> <lba_start>", file=sys.stderr)
        sys.exit(1)

    image_path = sys.argv[1]
    lba_start = int(sys.argv[2])

    # Define all assets
    assets = [
        ("white.raw",       gen_white_raw),
        ("grid.raw",        gen_grid_raw),
        ("flat_normal.raw", gen_flat_normal_raw),
        ("bump_normal.raw", gen_bump_normal_raw),
        ("cube.cobj",       gen_cube_cobj),
        ("quad.cobj",       gen_quad_cobj),
        ("sphere.cobj",     gen_sphere_cobj),
    ]

    with open(image_path, 'r+b') as f:
        writer = ChaosFSWriter(f, lba_start)

        # Create /test/ directory under root (inode 1)
        ROOT_INO = 1
        print("gen_assets: creating /test/")
        test_dir_ino = writer.create_directory(ROOT_INO, "test")

        # Generate and write each asset
        for name, gen_fn in assets:
            data = gen_fn()
            writer.create_file(test_dir_ino, name, data)
            print(f"gen_assets: wrote /test/{name} ({len(data)} bytes)")

        writer.flush()

    print(f"gen_assets: done ({writer.blocks_allocated} blocks, "
          f"{writer.inodes_allocated} inodes allocated)")


if __name__ == '__main__':
    main()
