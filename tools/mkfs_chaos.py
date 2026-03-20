#!/usr/bin/env python3
"""mkfs_chaos — Host-side ChaosFS format tool.
Usage: python mkfs_chaos.py <disk_image> <lba_start>
Writes a formatted ChaosFS at the given LBA offset in the disk image."""

import struct
import sys
import os

# Constants (must match chaos_types.h exactly)
CHAOS_MAGIC = 0x43484653       # 'CHFS'
CHAOS_VERSION = 2
CHAOS_BLOCK_SIZE = 4096
CHAOS_SECTORS_PER_BLK = 8
CHAOS_INODE_MAGIC = 0xC4A0
CHAOS_INODE_SIZE = 128
CHAOS_INODES_PER_BLOCK = CHAOS_BLOCK_SIZE // CHAOS_INODE_SIZE  # 32
CHAOS_DIRENT_SIZE = 64
CHAOS_TYPE_DIR = 0x2000
CHAOS_DT_DIR = 2

def crc32_chaos(data):
    """CRC-32/ISO-HDLC (same as zlib), polynomial 0xEDB88320."""
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1))
    return crc ^ 0xFFFFFFFF

def write_block(f, lba_start, block_idx, data):
    """Write a 4KB block to the disk image."""
    offset = (lba_start + block_idx * CHAOS_SECTORS_PER_BLK) * 512
    f.seek(offset)
    assert len(data) == CHAOS_BLOCK_SIZE
    f.write(data)

def make_superblock(total_blocks, bitmap_blocks, inode_count, inode_table_blocks, data_start):
    """Build a superblock as bytes."""
    # Pack all fields before checksum
    sb = bytearray(CHAOS_BLOCK_SIZE)
    offset = 0

    def pack_into(fmt, *args):
        nonlocal offset
        struct.pack_into(fmt, sb, offset, *args)
        offset += struct.calcsize(fmt)

    pack_into('<I', CHAOS_MAGIC)                    # magic
    pack_into('<I', CHAOS_VERSION)                   # version
    pack_into('16s', b'AIOS\x00' + b'\x00' * 11)   # fs_name[16]
    pack_into('<I', CHAOS_BLOCK_SIZE)                # block_size
    pack_into('<I', total_blocks)                    # total_blocks
    pack_into('<I', 2)                               # bitmap_start
    pack_into('<I', bitmap_blocks)                   # bitmap_blocks
    pack_into('<I', 2 + bitmap_blocks)               # inode_table_start
    pack_into('<I', inode_table_blocks)              # inode_table_blocks
    pack_into('<I', data_start)                      # data_start
    pack_into('<I', inode_count)                     # total_inodes
    pack_into('<I', total_blocks - data_start - 1)   # free_blocks
    pack_into('<I', inode_count - 2)                 # free_inodes
    pack_into('<B', 1)                               # clean_unmount
    pack_into('<B', 0)                               # mounted
    pack_into('<H', 0)                               # mount_count
    pack_into('<I', 0)                               # created_time
    pack_into('<I', 0)                               # last_mounted_time
    pack_into('<I', 0)                               # last_fsck_time
    pack_into('<I', 0)                               # journal_start
    pack_into('<I', 0)                               # journal_blocks
    offset += 28                                     # reserved[28]

    # Compute CRC over everything up to (not including) the checksum field
    checksum_offset = offset
    crc = crc32_chaos(sb[:checksum_offset])
    struct.pack_into('<I', sb, checksum_offset, crc)

    return bytes(sb)

def make_inode(mode, link_count, size, extent_start, extent_count_blks):
    """Build a 128-byte inode."""
    ino = bytearray(CHAOS_INODE_SIZE)
    struct.pack_into('<H', ino, 0, CHAOS_INODE_MAGIC)  # magic
    struct.pack_into('<H', ino, 2, mode)                # mode
    struct.pack_into('<I', ino, 4, link_count)          # link_count
    # open_count(4) + unlink_pending(1) + pad0(3) = 8 bytes at offset 8, all zero
    struct.pack_into('<Q', ino, 16, size)               # size (uint64_t at offset 16)
    # timestamps at 24,28,32 — all zero
    # flags at 36 — zero
    struct.pack_into('<B', ino, 40, 1)                  # extent_count = 1
    # has_indirect(1) + reserved(2) at 41-43 — zero
    # First extent at offset 44
    struct.pack_into('<I', ino, 44, extent_start)       # extents[0].start_block
    struct.pack_into('<I', ino, 48, extent_count_blks)  # extents[0].block_count
    # Remaining extents + indirect_block + pad1 — all zero
    return bytes(ino)

def make_dirent(inode_num, dtype, name):
    """Build a 64-byte directory entry."""
    d = bytearray(CHAOS_DIRENT_SIZE)
    name_bytes = name.encode('ascii')
    struct.pack_into('<I', d, 0, inode_num)         # inode
    struct.pack_into('<B', d, 4, dtype)             # type
    struct.pack_into('<B', d, 5, len(name_bytes))   # name_len
    d[6:6+len(name_bytes)] = name_bytes             # name (within 54-byte field)
    return bytes(d)

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <disk_image> <lba_start>", file=sys.stderr)
        sys.exit(1)

    image_path = sys.argv[1]
    lba_start = int(sys.argv[2])

    file_size = os.path.getsize(image_path)
    fs_offset = lba_start * 512
    if fs_offset >= file_size:
        print(f"Error: LBA start {lba_start} exceeds file size", file=sys.stderr)
        sys.exit(1)

    lba_count = (file_size - fs_offset) // 512
    total_blocks = (lba_count * 512) // CHAOS_BLOCK_SIZE

    if total_blocks < 64:
        print(f"Error: FS region too small: {total_blocks} blocks (need >= 64)", file=sys.stderr)
        sys.exit(1)

    # Compute layout
    bitmap_blocks = (total_blocks + 32767) // 32768
    inode_count = max(total_blocks // 4, 32)
    inode_table_blocks = (inode_count + CHAOS_INODES_PER_BLOCK - 1) // CHAOS_INODES_PER_BLOCK
    data_start = 2 + bitmap_blocks + inode_table_blocks

    print(f"mkfs_chaos: {total_blocks} blocks, bitmap={bitmap_blocks}, "
          f"inodes={inode_count} ({inode_table_blocks} blocks), data_start={data_start}")

    with open(image_path, 'r+b') as f:
        zero_block = b'\x00' * CHAOS_BLOCK_SIZE

        # Zero metadata region
        for b in range(data_start):
            write_block(f, lba_start, b, zero_block)

        # Build and write bitmap
        bitmap_bytes = bytearray(bitmap_blocks * CHAOS_BLOCK_SIZE)
        # Mark metadata blocks (0..data_start-1) as used
        for b in range(data_start):
            bitmap_bytes[b // 8] |= (1 << (b % 8))
        # Mark root directory data block as used
        root_data = data_start
        bitmap_bytes[root_data // 8] |= (1 << (root_data % 8))

        for i in range(bitmap_blocks):
            chunk = bitmap_bytes[i*CHAOS_BLOCK_SIZE:(i+1)*CHAOS_BLOCK_SIZE]
            write_block(f, lba_start, 2 + i, bytes(chunk))

        # Write root inode (inode 1 in first inode table block)
        inode_block = bytearray(CHAOS_BLOCK_SIZE)
        root_ino = make_inode(CHAOS_TYPE_DIR | 0o755, 2, CHAOS_BLOCK_SIZE, root_data, 1)
        inode_block[CHAOS_INODE_SIZE:CHAOS_INODE_SIZE*2] = root_ino  # slot 1
        write_block(f, lba_start, 2 + bitmap_blocks, bytes(inode_block))

        # Write root directory data block (. and ..)
        dir_block = bytearray(CHAOS_BLOCK_SIZE)
        dir_block[0:CHAOS_DIRENT_SIZE] = make_dirent(1, CHAOS_DT_DIR, ".")
        dir_block[CHAOS_DIRENT_SIZE:CHAOS_DIRENT_SIZE*2] = make_dirent(1, CHAOS_DT_DIR, "..")
        write_block(f, lba_start, root_data, bytes(dir_block))

        # Write superblock to block 0 and block 1
        sb = make_superblock(total_blocks, bitmap_blocks, inode_count, inode_table_blocks, data_start)
        write_block(f, lba_start, 0, sb)
        write_block(f, lba_start, 1, sb)

    print(f"mkfs_chaos: format complete ({total_blocks} blocks, "
          f"{total_blocks - data_start - 1} free, {inode_count} inodes)")

if __name__ == '__main__':
    main()
