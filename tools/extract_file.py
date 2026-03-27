#!/usr/bin/env python3
"""Extract a file from ChaosFS disk image.
Usage: python3 tools/extract_file.py build/os.img /screenshot.bmp screenshot.bmp"""

import sys, struct

BLOCK_SIZE = 4096
CHAOSFS_LBA = 67584

def read_block(f, base, blk):
    f.seek(base + blk * BLOCK_SIZE)
    return f.read(BLOCK_SIZE)

def extract_file(image_path, chaosfs_path, output_path):
    with open(image_path, 'rb') as f:
        base = CHAOSFS_LBA * 512

        # Superblock (block 0)
        sb = read_block(f, base, 0)
        magic = struct.unpack_from('<I', sb, 0)[0]
        if magic != 0x43484653:
            print(f"Bad magic: 0x{magic:08x}")
            return False

        inode_start = struct.unpack_from('<I', sb, 40)[0]  # inode_table_start

        # Walk path from root inode 1
        parts = [p for p in chaosfs_path.split('/') if p]
        cur = 1  # root inode

        for part in parts:
            # Read current inode
            ino_off = cur * 128
            ino_blk = inode_start + ino_off // BLOCK_SIZE
            ino_rem = ino_off % BLOCK_SIZE
            idata = read_block(f, base, ino_blk)
            ino = idata[ino_rem:ino_rem+128]

            mode = struct.unpack_from('<H', ino, 2)[0]
            if not (mode & 0x2000):
                print(f"Not a directory at '{part}'")
                return False

            # Read extents
            ext_count = ino[40]
            found = False
            for ei in range(min(ext_count, 6)):
                es = struct.unpack_from('<I', ino, 44 + ei*8)[0]
                ec = struct.unpack_from('<I', ino, 48 + ei*8)[0]
                for bi in range(ec):
                    blk = read_block(f, base, es + bi)
                    for di in range(BLOCK_SIZE // 64):
                        d = blk[di*64:(di+1)*64]
                        d_ino = struct.unpack_from('<I', d, 0)[0]
                        d_nlen = d[5]
                        d_name = d[6:6+d_nlen].decode('utf-8', errors='replace') if d_nlen else ''
                        if d_ino > 0 and d_name == part:
                            cur = d_ino
                            found = True
                            break
                    if found: break
                if found: break
            if not found:
                print(f"Not found: '{part}'")
                return False

        # Read target file inode
        ino_off = cur * 128
        ino_blk = inode_start + ino_off // BLOCK_SIZE
        ino_rem = ino_off % BLOCK_SIZE
        idata = read_block(f, base, ino_blk)
        ino = idata[ino_rem:ino_rem+128]

        mode = struct.unpack_from('<H', ino, 2)[0]
        size = struct.unpack_from('<Q', ino, 16)[0]
        ext_count = ino[40]

        if mode & 0x2000:
            print(f"'{chaosfs_path}' is a directory")
            return False

        # Read data from extents
        data = bytearray()
        for ei in range(min(ext_count, 6)):
            es = struct.unpack_from('<I', ino, 44 + ei*8)[0]
            ec = struct.unpack_from('<I', ino, 48 + ei*8)[0]
            for bi in range(ec):
                data.extend(read_block(f, base, es + bi))

        data = bytes(data[:size])
        with open(output_path, 'wb') as out:
            out.write(data)

        print(f"OK: {chaosfs_path} ({size} bytes) -> {output_path}")
        return True

if __name__ == '__main__':
    if len(sys.argv) != 4:
        print("Usage: python3 tools/extract_file.py <disk.img> </chaosfs/path> <output>")
        sys.exit(1)
    sys.exit(0 if extract_file(sys.argv[1], sys.argv[2], sys.argv[3]) else 1)
