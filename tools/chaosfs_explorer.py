#!/usr/bin/env python3
"""ChaosFS Explorer — GUI tool for browsing and editing ChaosFS disk images.
Tkinter desktop app with file browser tree, import/export, create/delete/rename."""

import struct
import os
import sys
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, simpledialog

# ── ChaosFS Constants ──────────────────────────────────────────────

CHAOS_MAGIC = 0x43484653
CHAOS_VERSION = 2
CHAOS_BLOCK_SIZE = 4096
CHAOS_SECTORS_PER_BLK = 8
CHAOS_INODE_MAGIC = 0xC4A0
CHAOS_INODE_SIZE = 128
CHAOS_INODES_PER_BLOCK = CHAOS_BLOCK_SIZE // CHAOS_INODE_SIZE
CHAOS_DIRENT_SIZE = 64
CHAOS_DIRENTS_PER_BLK = CHAOS_BLOCK_SIZE // CHAOS_DIRENT_SIZE
CHAOS_MAX_INLINE_EXTENTS = 6
CHAOS_TYPE_FILE = 0x1000
CHAOS_TYPE_DIR = 0x2000
CHAOS_TYPE_MASK = 0xF000
CHAOS_DT_FILE = 1
CHAOS_DT_DIR = 2
CHAOS_INODE_NULL = 0
CHAOS_BLOCK_NULL = 0

# ── ChaosFS Disk Interface ────────────────────────────────────────

class ChaosFS:
    def __init__(self, image_path, lba_start=67584):
        self.image_path = image_path
        self.lba_start = lba_start
        self.f = None
        self.sb = None

    def open(self):
        self.f = open(self.image_path, 'r+b')
        self._read_superblock()

    def close(self):
        if self.f:
            self.f.close()
            self.f = None

    def _block_offset(self, block_idx):
        return (self.lba_start + block_idx * CHAOS_SECTORS_PER_BLK) * 512

    def read_block(self, block_idx):
        self.f.seek(self._block_offset(block_idx))
        return self.f.read(CHAOS_BLOCK_SIZE)

    def write_block(self, block_idx, data):
        assert len(data) == CHAOS_BLOCK_SIZE
        self.f.seek(self._block_offset(block_idx))
        self.f.write(data)
        self.f.flush()

    def _read_superblock(self):
        data = self.read_block(0)
        magic = struct.unpack_from('<I', data, 0)[0]
        if magic != CHAOS_MAGIC:
            raise ValueError(f"Bad ChaosFS magic: 0x{magic:08X}")
        self.sb = {
            'magic': magic,
            'version': struct.unpack_from('<I', data, 4)[0],
            'fs_name': data[8:24].split(b'\x00')[0].decode('ascii', errors='replace'),
            'block_size': struct.unpack_from('<I', data, 24)[0],
            'total_blocks': struct.unpack_from('<I', data, 28)[0],
            'bitmap_start': struct.unpack_from('<I', data, 32)[0],
            'bitmap_blocks': struct.unpack_from('<I', data, 36)[0],
            'inode_table_start': struct.unpack_from('<I', data, 40)[0],
            'inode_table_blocks': struct.unpack_from('<I', data, 44)[0],
            'data_start': struct.unpack_from('<I', data, 48)[0],
            'total_inodes': struct.unpack_from('<I', data, 52)[0],
            'free_blocks': struct.unpack_from('<I', data, 56)[0],
            'free_inodes': struct.unpack_from('<I', data, 60)[0],
        }

    def read_inode(self, inode_num):
        block_in_table = inode_num // CHAOS_INODES_PER_BLOCK
        slot_in_block = inode_num % CHAOS_INODES_PER_BLOCK
        data = self.read_block(self.sb['inode_table_start'] + block_in_table)
        off = slot_in_block * CHAOS_INODE_SIZE
        ino_data = data[off:off + CHAOS_INODE_SIZE]

        magic = struct.unpack_from('<H', ino_data, 0)[0]
        if magic != CHAOS_INODE_MAGIC:
            return None

        extents = []
        for i in range(CHAOS_MAX_INLINE_EXTENTS):
            eoff = 44 + i * 8
            start = struct.unpack_from('<I', ino_data, eoff)[0]
            count = struct.unpack_from('<I', ino_data, eoff + 4)[0]
            if count > 0:
                extents.append((start, count))

        return {
            'magic': magic,
            'mode': struct.unpack_from('<H', ino_data, 2)[0],
            'link_count': struct.unpack_from('<I', ino_data, 4)[0],
            'size': struct.unpack_from('<Q', ino_data, 16)[0],
            'extent_count': ino_data[40],
            'extents': extents,
        }

    def inode_logical_block(self, inode, logical):
        pos = 0
        for start, count in inode['extents']:
            if logical < pos + count:
                return start + (logical - pos)
            pos += count
        return CHAOS_BLOCK_NULL

    def read_dir(self, inode_num):
        inode = self.read_inode(inode_num)
        if not inode or (inode['mode'] & CHAOS_TYPE_MASK) != CHAOS_TYPE_DIR:
            return []

        entries = []
        total_blocks = sum(c for _, c in inode['extents'])
        for b in range(total_blocks):
            phys = self.inode_logical_block(inode, b)
            if phys == CHAOS_BLOCK_NULL:
                continue
            data = self.read_block(phys)
            for s in range(CHAOS_DIRENTS_PER_BLK):
                off = s * CHAOS_DIRENT_SIZE
                d_inode = struct.unpack_from('<I', data, off)[0]
                if d_inode == CHAOS_INODE_NULL:
                    continue
                d_type = data[off + 4]
                d_name_len = data[off + 5]
                d_name = data[off + 6:off + 6 + d_name_len].decode('ascii', errors='replace')
                entries.append({
                    'inode': d_inode,
                    'type': d_type,
                    'name': d_name,
                    'block_idx': b,
                    'slot_idx': s,
                })
        return entries

    def read_file(self, inode_num):
        inode = self.read_inode(inode_num)
        if not inode:
            return b''
        size = inode['size']
        result = bytearray()
        total_blocks = sum(c for _, c in inode['extents'])
        for b in range(total_blocks):
            phys = self.inode_logical_block(inode, b)
            if phys == CHAOS_BLOCK_NULL:
                result.extend(b'\x00' * CHAOS_BLOCK_SIZE)
            else:
                result.extend(self.read_block(phys))
        return bytes(result[:size])

    def _alloc_block(self):
        """Allocate a free block from the bitmap."""
        for bi in range(self.sb['bitmap_blocks']):
            bdata = bytearray(self.read_block(self.sb['bitmap_start'] + bi))
            for word_off in range(0, CHAOS_BLOCK_SIZE, 4):
                word = struct.unpack_from('<I', bdata, word_off)[0]
                if word == 0xFFFFFFFF:
                    continue
                for bit in range(32):
                    if not (word & (1 << bit)):
                        block_idx = (bi * CHAOS_BLOCK_SIZE * 8) + (word_off * 8) + bit
                        if block_idx < self.sb['data_start'] or block_idx >= self.sb['total_blocks']:
                            continue
                        # Set bit
                        bdata[word_off + bit // 8] |= (1 << (bit % 8))
                        self.write_block(self.sb['bitmap_start'] + bi, bytes(bdata))
                        self.sb['free_blocks'] -= 1
                        return block_idx
        return CHAOS_BLOCK_NULL

    def _free_block(self, block_idx):
        """Free a block in the bitmap."""
        bitmap_byte_offset = block_idx // 8
        bitmap_block = bitmap_byte_offset // CHAOS_BLOCK_SIZE
        offset_in_block = bitmap_byte_offset % CHAOS_BLOCK_SIZE
        bit_in_byte = block_idx % 8

        bdata = bytearray(self.read_block(self.sb['bitmap_start'] + bitmap_block))
        bdata[offset_in_block] &= ~(1 << bit_in_byte)
        self.write_block(self.sb['bitmap_start'] + bitmap_block, bytes(bdata))
        self.sb['free_blocks'] += 1

    def _alloc_inode(self):
        """Find a free inode slot."""
        for b in range(self.sb['inode_table_blocks']):
            data = self.read_block(self.sb['inode_table_start'] + b)
            for s in range(CHAOS_INODES_PER_BLOCK):
                ino_num = b * CHAOS_INODES_PER_BLOCK + s
                if ino_num == 0:
                    continue
                off = s * CHAOS_INODE_SIZE
                magic = struct.unpack_from('<H', data, off)[0]
                if magic != CHAOS_INODE_MAGIC:
                    self.sb['free_inodes'] -= 1
                    return ino_num
        return CHAOS_INODE_NULL

    def _write_inode(self, inode_num, inode_bytes):
        block_in_table = inode_num // CHAOS_INODES_PER_BLOCK
        slot_in_block = inode_num % CHAOS_INODES_PER_BLOCK
        data = bytearray(self.read_block(self.sb['inode_table_start'] + block_in_table))
        off = slot_in_block * CHAOS_INODE_SIZE
        data[off:off + CHAOS_INODE_SIZE] = inode_bytes
        self.write_block(self.sb['inode_table_start'] + block_in_table, bytes(data))

    def _make_inode_bytes(self, mode, link_count, size, extents):
        ino = bytearray(CHAOS_INODE_SIZE)
        struct.pack_into('<H', ino, 0, CHAOS_INODE_MAGIC)
        struct.pack_into('<H', ino, 2, mode)
        struct.pack_into('<I', ino, 4, link_count)
        struct.pack_into('<Q', ino, 16, size)
        struct.pack_into('<B', ino, 40, len(extents))
        for i, (start, count) in enumerate(extents):
            struct.pack_into('<I', ino, 44 + i * 8, start)
            struct.pack_into('<I', ino, 48 + i * 8, count)
        return bytes(ino)

    def _add_dirent(self, dir_inode_num, name, target_inode, dtype):
        """Add a directory entry to a directory."""
        inode = self.read_inode(dir_inode_num)
        if not inode:
            return False
        total_blocks = sum(c for _, c in inode['extents'])

        name_bytes = name.encode('ascii')[:53]

        # Search for free slot in existing blocks
        for b in range(total_blocks):
            phys = self.inode_logical_block(inode, b)
            if phys == CHAOS_BLOCK_NULL:
                continue
            data = bytearray(self.read_block(phys))
            for s in range(CHAOS_DIRENTS_PER_BLK):
                off = s * CHAOS_DIRENT_SIZE
                if struct.unpack_from('<I', data, off)[0] == CHAOS_INODE_NULL:
                    struct.pack_into('<I', data, off, target_inode)
                    data[off + 4] = dtype
                    data[off + 5] = len(name_bytes)
                    data[off + 6:off + 6 + len(name_bytes)] = name_bytes
                    self.write_block(phys, bytes(data))
                    return True

        # Need new block for directory
        new_block = self._alloc_block()
        if new_block == CHAOS_BLOCK_NULL:
            return False

        data = bytearray(CHAOS_BLOCK_SIZE)
        struct.pack_into('<I', data, 0, target_inode)
        data[4] = dtype
        data[5] = len(name_bytes)
        data[6:6 + len(name_bytes)] = name_bytes
        self.write_block(new_block, bytes(data))

        # Add extent to directory inode (simplified — just add new extent)
        extents = list(inode['extents'])
        extents.append((new_block, 1))
        new_size = inode['size'] + CHAOS_BLOCK_SIZE
        ino_bytes = self._make_inode_bytes(inode['mode'], inode['link_count'], new_size, extents)
        self._write_inode(dir_inode_num, ino_bytes)
        return True

    def _remove_dirent(self, dir_inode_num, name):
        """Remove a directory entry by name."""
        inode = self.read_inode(dir_inode_num)
        if not inode:
            return None
        total_blocks = sum(c for _, c in inode['extents'])
        name_bytes = name.encode('ascii')

        for b in range(total_blocks):
            phys = self.inode_logical_block(inode, b)
            if phys == CHAOS_BLOCK_NULL:
                continue
            data = bytearray(self.read_block(phys))
            for s in range(CHAOS_DIRENTS_PER_BLK):
                off = s * CHAOS_DIRENT_SIZE
                d_inode = struct.unpack_from('<I', data, off)[0]
                if d_inode == CHAOS_INODE_NULL:
                    continue
                d_name_len = data[off + 5]
                d_name = data[off + 6:off + 6 + d_name_len]
                if d_name == name_bytes:
                    # Clear entry
                    data[off:off + CHAOS_DIRENT_SIZE] = b'\x00' * CHAOS_DIRENT_SIZE
                    self.write_block(phys, bytes(data))
                    return d_inode
        return None

    def write_file(self, dir_inode_num, name, content):
        """Create a new file with given content in the specified directory."""
        ino_num = self._alloc_inode()
        if ino_num == CHAOS_INODE_NULL:
            return False

        # Allocate blocks for content
        needed_blocks = (len(content) + CHAOS_BLOCK_SIZE - 1) // CHAOS_BLOCK_SIZE
        if needed_blocks == 0:
            needed_blocks = 0  # empty file is valid

        extents = []
        blocks = []
        for _ in range(needed_blocks):
            blk = self._alloc_block()
            if blk == CHAOS_BLOCK_NULL:
                # Free already allocated
                for b in blocks:
                    self._free_block(b)
                return False
            blocks.append(blk)

        # Build extents from consecutive blocks
        if blocks:
            ext_start = blocks[0]
            ext_count = 1
            for i in range(1, len(blocks)):
                if blocks[i] == blocks[i-1] + 1:
                    ext_count += 1
                else:
                    extents.append((ext_start, ext_count))
                    ext_start = blocks[i]
                    ext_count = 1
            extents.append((ext_start, ext_count))

        # Write data blocks
        for i, blk in enumerate(blocks):
            chunk = content[i * CHAOS_BLOCK_SIZE:(i + 1) * CHAOS_BLOCK_SIZE]
            if len(chunk) < CHAOS_BLOCK_SIZE:
                chunk = chunk + b'\x00' * (CHAOS_BLOCK_SIZE - len(chunk))
            self.write_block(blk, chunk)

        # Write inode
        ino_bytes = self._make_inode_bytes(CHAOS_TYPE_FILE | 0o644, 1, len(content), extents)
        self._write_inode(ino_num, ino_bytes)

        # Add directory entry
        self._add_dirent(dir_inode_num, name, ino_num, CHAOS_DT_FILE)
        self._update_superblock()
        return True

    def mkdir(self, parent_inode_num, name):
        """Create a new directory."""
        ino_num = self._alloc_inode()
        if ino_num == CHAOS_INODE_NULL:
            return False

        data_block = self._alloc_block()
        if data_block == CHAOS_BLOCK_NULL:
            return False

        # Write . and .. entries
        data = bytearray(CHAOS_BLOCK_SIZE)
        struct.pack_into('<I', data, 0, ino_num)
        data[4] = CHAOS_DT_DIR
        data[5] = 1
        data[6] = ord('.')

        struct.pack_into('<I', data, CHAOS_DIRENT_SIZE, parent_inode_num)
        data[CHAOS_DIRENT_SIZE + 4] = CHAOS_DT_DIR
        data[CHAOS_DIRENT_SIZE + 5] = 2
        data[CHAOS_DIRENT_SIZE + 6] = ord('.')
        data[CHAOS_DIRENT_SIZE + 7] = ord('.')

        self.write_block(data_block, bytes(data))

        # Write inode
        ino_bytes = self._make_inode_bytes(CHAOS_TYPE_DIR | 0o755, 2, CHAOS_BLOCK_SIZE, [(data_block, 1)])
        self._write_inode(ino_num, ino_bytes)

        # Add to parent
        self._add_dirent(parent_inode_num, name, ino_num, CHAOS_DT_DIR)

        # Increment parent link count
        parent = self.read_inode(parent_inode_num)
        if parent:
            p_bytes = self._make_inode_bytes(parent['mode'], parent['link_count'] + 1,
                                              parent['size'], parent['extents'])
            self._write_inode(parent_inode_num, p_bytes)

        self._update_superblock()
        return True

    def delete_file(self, parent_inode_num, name):
        """Delete a file from a directory."""
        target_ino = self._remove_dirent(parent_inode_num, name)
        if target_ino is None:
            return False

        inode = self.read_inode(target_ino)
        if not inode:
            return False

        # Free data blocks
        for start, count in inode['extents']:
            for b in range(count):
                self._free_block(start + b)

        # Zero the inode
        self._write_inode(target_ino, b'\x00' * CHAOS_INODE_SIZE)
        self.sb['free_inodes'] += 1
        self._update_superblock()
        return True

    def rename_entry(self, dir_inode_num, old_name, new_name):
        """Rename a directory entry."""
        inode = self.read_inode(dir_inode_num)
        if not inode:
            return False
        total_blocks = sum(c for _, c in inode['extents'])
        old_bytes = old_name.encode('ascii')
        new_bytes = new_name.encode('ascii')[:53]

        for b in range(total_blocks):
            phys = self.inode_logical_block(inode, b)
            if phys == CHAOS_BLOCK_NULL:
                continue
            data = bytearray(self.read_block(phys))
            for s in range(CHAOS_DIRENTS_PER_BLK):
                off = s * CHAOS_DIRENT_SIZE
                d_inode = struct.unpack_from('<I', data, off)[0]
                if d_inode == CHAOS_INODE_NULL:
                    continue
                d_name_len = data[off + 5]
                d_name = data[off + 6:off + 6 + d_name_len]
                if d_name == old_bytes:
                    data[off + 5] = len(new_bytes)
                    data[off + 6:off + 60] = b'\x00' * 54
                    data[off + 6:off + 6 + len(new_bytes)] = new_bytes
                    self.write_block(phys, bytes(data))
                    return True
        return False

    def _update_superblock(self):
        """Rewrite superblock with updated free counts."""
        data = bytearray(self.read_block(0))
        struct.pack_into('<I', data, 56, self.sb['free_blocks'])
        struct.pack_into('<I', data, 60, self.sb['free_inodes'])
        # Recompute CRC
        crc_offset = len(data)
        # Find checksum field offset (after reserved[28])
        # magic(4)+version(4)+name(16)+block_size(4)+total(4)+bitmap_start(4)+bitmap_blocks(4)
        # +inode_start(4)+inode_blocks(4)+data_start(4)+total_inodes(4)+free_blocks(4)+free_inodes(4)
        # +clean(1)+mounted(1)+mount_count(2)+created(4)+last_mounted(4)+last_fsck(4)
        # +journal_start(4)+journal_blocks(4)+reserved(28) = 128 bytes before checksum
        crc_data = data[:128]
        crc = 0xFFFFFFFF
        for b in crc_data:
            crc ^= b
            for _ in range(8):
                crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1))
        crc ^= 0xFFFFFFFF
        struct.pack_into('<I', data, 128, crc)
        self.write_block(0, bytes(data))
        self.write_block(1, bytes(data))

    def resolve_path(self, path):
        """Resolve a path to (parent_inode, name, target_inode). Returns None if not found."""
        if not path or path[0] != '/':
            return None
        if path == '/':
            return (1, '/', 1)

        parts = [p for p in path.split('/') if p]
        current = 1  # root

        for i, part in enumerate(parts):
            entries = self.read_dir(current)
            found = None
            for e in entries:
                if e['name'] == part:
                    found = e
                    break
            if found is None:
                if i == len(parts) - 1:
                    return (current, part, None)  # parent exists, target doesn't
                return None
            if i == len(parts) - 1:
                return (current, part, found['inode'])
            current = found['inode']

        return (current, '', current)


# ── GUI Application ───────────────────────────────────────────────

class ChaosExplorer(tk.Tk):
    def __init__(self, image_path=None):
        super().__init__()
        self.title("ChaosFS Explorer")
        self.geometry("900x600")
        self.fs = None
        self.current_dir_inode = 1
        self.current_path = "/"

        self._build_ui()

        if image_path:
            self._open_image(image_path)

    def _build_ui(self):
        # Toolbar
        toolbar = ttk.Frame(self)
        toolbar.pack(fill=tk.X, padx=5, pady=5)

        ttk.Button(toolbar, text="Open Image", command=self._on_open).pack(side=tk.LEFT, padx=2)
        ttk.Button(toolbar, text="Import File", command=self._on_import).pack(side=tk.LEFT, padx=2)
        ttk.Button(toolbar, text="Export File", command=self._on_export).pack(side=tk.LEFT, padx=2)
        ttk.Button(toolbar, text="New Folder", command=self._on_mkdir).pack(side=tk.LEFT, padx=2)
        ttk.Button(toolbar, text="Delete", command=self._on_delete).pack(side=tk.LEFT, padx=2)
        ttk.Button(toolbar, text="Rename", command=self._on_rename).pack(side=tk.LEFT, padx=2)
        ttk.Button(toolbar, text="Refresh", command=self._refresh).pack(side=tk.LEFT, padx=2)

        # Path bar
        path_frame = ttk.Frame(self)
        path_frame.pack(fill=tk.X, padx=5)
        ttk.Label(path_frame, text="Path:").pack(side=tk.LEFT)
        self.path_var = tk.StringVar(value="/")
        self.path_label = ttk.Label(path_frame, textvariable=self.path_var, font=("Consolas", 11, "bold"))
        self.path_label.pack(side=tk.LEFT, padx=5)

        # Info bar
        self.info_var = tk.StringVar(value="No image loaded")
        ttk.Label(self, textvariable=self.info_var, relief=tk.SUNKEN, anchor=tk.W).pack(fill=tk.X, side=tk.BOTTOM, padx=5, pady=2)

        # Tree view
        columns = ("type", "size", "inode", "blocks")
        self.tree = ttk.Treeview(self, columns=columns, show="tree headings", selectmode="browse")
        self.tree.heading("#0", text="Name", anchor=tk.W)
        self.tree.heading("type", text="Type")
        self.tree.heading("size", text="Size")
        self.tree.heading("inode", text="Inode")
        self.tree.heading("blocks", text="Blocks")
        self.tree.column("#0", width=300)
        self.tree.column("type", width=60)
        self.tree.column("size", width=100)
        self.tree.column("inode", width=60)
        self.tree.column("blocks", width=60)
        self.tree.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)
        self.tree.bind("<Double-1>", self._on_double_click)

    def _open_image(self, path):
        try:
            if self.fs:
                self.fs.close()
            self.fs = ChaosFS(path)
            self.fs.open()
            sb = self.fs.sb
            self.info_var.set(f"{os.path.basename(path)} | "
                            f"Label: {sb['fs_name']} | "
                            f"Blocks: {sb['total_blocks']} ({sb['total_blocks'] * 4 // 1024} MB) | "
                            f"Free: {sb['free_blocks']} | "
                            f"Inodes: {sb['total_inodes']} (free: {sb['free_inodes']})")
            self.current_dir_inode = 1
            self.current_path = "/"
            self._refresh()
        except Exception as e:
            messagebox.showerror("Error", f"Failed to open image:\n{e}")

    def _refresh(self):
        if not self.fs:
            return
        self.tree.delete(*self.tree.get_children())
        self.path_var.set(self.current_path)

        # Add ".." entry if not root
        if self.current_dir_inode != 1:
            self.tree.insert("", 0, text="..", values=("DIR", "", "", ""), tags=("dir",))

        entries = self.fs.read_dir(self.current_dir_inode)
        for e in sorted(entries, key=lambda x: (x['type'] != CHAOS_DT_DIR, x['name'])):
            if e['name'] in ('.', '..'):
                continue
            ino = self.fs.read_inode(e['inode'])
            if not ino:
                continue
            is_dir = e['type'] == CHAOS_DT_DIR
            size_str = "" if is_dir else self._format_size(ino['size'])
            type_str = "DIR" if is_dir else "FILE"
            blocks = sum(c for _, c in ino['extents'])
            self.tree.insert("", tk.END, text=e['name'],
                           values=(type_str, size_str, e['inode'], blocks),
                           tags=("dir" if is_dir else "file",))

        self.tree.tag_configure("dir", foreground="#0066CC")

    def _format_size(self, size):
        if size < 1024:
            return f"{size} B"
        elif size < 1024 * 1024:
            return f"{size / 1024:.1f} KB"
        else:
            return f"{size / (1024 * 1024):.1f} MB"

    def _get_selected(self):
        sel = self.tree.selection()
        if not sel:
            return None, None
        item = self.tree.item(sel[0])
        return item['text'], item['values']

    def _on_open(self):
        path = filedialog.askopenfilename(
            title="Open Disk Image",
            filetypes=[("Disk Images", "*.img"), ("All Files", "*.*")])
        if path:
            self._open_image(path)

    def _on_double_click(self, event):
        name, values = self._get_selected()
        if not name:
            return

        if name == "..":
            # Go up
            if self.current_path != "/":
                parts = self.current_path.rstrip('/').rsplit('/', 1)
                parent_path = parts[0] if parts[0] else "/"
                result = self.fs.resolve_path(parent_path)
                if result:
                    self.current_dir_inode = result[2]
                    self.current_path = parent_path
                    self._refresh()
            return

        if values and values[0] == "DIR":
            new_path = self.current_path.rstrip('/') + '/' + name
            result = self.fs.resolve_path(new_path)
            if result and result[2]:
                self.current_dir_inode = result[2]
                self.current_path = new_path
                self._refresh()

    def _on_import(self):
        if not self.fs:
            messagebox.showwarning("Warning", "No image loaded")
            return

        paths = filedialog.askopenfilenames(title="Import Files")
        if not paths:
            return

        for path in paths:
            name = os.path.basename(path)
            try:
                with open(path, 'rb') as f:
                    content = f.read()
                if self.fs.write_file(self.current_dir_inode, name, content):
                    pass  # success
                else:
                    messagebox.showerror("Error", f"Failed to write {name}")
            except Exception as e:
                messagebox.showerror("Error", f"Failed to import {name}:\n{e}")

        self._refresh()
        self.fs._read_superblock()  # refresh counts
        self._update_info()

    def _on_export(self):
        if not self.fs:
            return
        name, values = self._get_selected()
        if not name or not values or values[0] != "FILE":
            messagebox.showwarning("Warning", "Select a file to export")
            return

        inode_num = int(values[2])
        save_path = filedialog.asksaveasfilename(title="Export File", initialfile=name)
        if not save_path:
            return

        try:
            content = self.fs.read_file(inode_num)
            with open(save_path, 'wb') as f:
                f.write(content)
            messagebox.showinfo("Success", f"Exported {name} ({len(content)} bytes)")
        except Exception as e:
            messagebox.showerror("Error", f"Failed to export:\n{e}")

    def _on_mkdir(self):
        if not self.fs:
            return
        name = simpledialog.askstring("New Folder", "Folder name:")
        if not name:
            return
        if self.fs.mkdir(self.current_dir_inode, name):
            self._refresh()
        else:
            messagebox.showerror("Error", "Failed to create directory")

    def _on_delete(self):
        if not self.fs:
            return
        name, values = self._get_selected()
        if not name or name == "..":
            return

        if not messagebox.askyesno("Confirm", f"Delete '{name}'?"):
            return

        if values[0] == "FILE":
            if self.fs.delete_file(self.current_dir_inode, name):
                self._refresh()
            else:
                messagebox.showerror("Error", "Failed to delete file")
        else:
            messagebox.showwarning("Warning", "Directory deletion not yet supported in GUI.\nUse rmdir from kernel shell.")

    def _on_rename(self):
        if not self.fs:
            return
        name, values = self._get_selected()
        if not name or name == "..":
            return
        new_name = simpledialog.askstring("Rename", "New name:", initialvalue=name)
        if not new_name or new_name == name:
            return
        if self.fs.rename_entry(self.current_dir_inode, name, new_name):
            self._refresh()
        else:
            messagebox.showerror("Error", "Failed to rename")

    def _update_info(self):
        if self.fs and self.fs.sb:
            sb = self.fs.sb
            self.info_var.set(f"{os.path.basename(self.fs.image_path)} | "
                            f"Label: {sb['fs_name']} | "
                            f"Blocks: {sb['total_blocks']} ({sb['total_blocks'] * 4 // 1024} MB) | "
                            f"Free: {sb['free_blocks']} | "
                            f"Inodes: {sb['total_inodes']} (free: {sb['free_inodes']})")


def main():
    image_path = None
    if len(sys.argv) > 1:
        image_path = sys.argv[1]

    app = ChaosExplorer(image_path)
    app.mainloop()


if __name__ == '__main__':
    main()
