#!/usr/bin/env python3
"""AIOS UEFI Disk Image Builder

Creates a 512MB GPT disk image with:
  - Protective MBR
  - GPT header + partition entries
  - ESP partition (FAT32, 32MB) containing BOOTX64.EFI and kernel.elf
  - ChaosFS partition (remaining space, formatted by populate_fs.py)

Usage: python3 tools/build_disk.py <output.img> <bootx64.efi> <kernel.elf>

Disk layout (512-byte sectors):
  LBA 0:           Protective MBR
  LBA 1:           Primary GPT Header
  LBA 2-33:        Primary GPT Partition Entries (128 entries)
  LBA 2048-67583:  ESP (FAT32, 32MB = 65536 sectors)
  LBA 67584+:      ChaosFS partition
  Last 33 LBAs:    Backup GPT
"""

import struct
import sys
import os
import uuid
import zlib

SECTOR_SIZE = 512
DISK_SIZE = 512 * 1024 * 1024  # 512 MB
TOTAL_SECTORS = DISK_SIZE // SECTOR_SIZE

# Partition layout
ESP_START_LBA = 2048        # 1MB-aligned
ESP_SECTORS = 65536         # 32MB
ESP_END_LBA = ESP_START_LBA + ESP_SECTORS - 1
CHAOSFS_START_LBA = ESP_START_LBA + ESP_SECTORS  # 67584

# GPT constants
GPT_HEADER_LBA = 1
GPT_ENTRIES_LBA = 2
GPT_ENTRY_SIZE = 128
GPT_ENTRIES_COUNT = 128
GPT_ENTRIES_SECTORS = (GPT_ENTRIES_COUNT * GPT_ENTRY_SIZE) // SECTOR_SIZE  # 32

# GUID types
EFI_SYSTEM_PARTITION_GUID = uuid.UUID('C12A7328-F81F-11D2-BA4B-00A0C93EC93B')
LINUX_FILESYSTEM_GUID = uuid.UUID('0FC63DAF-8483-4772-8E79-3D69D8477DE4')


def guid_to_mixed_endian(u):
    """Convert UUID to GPT mixed-endian format."""
    b = u.bytes
    # GPT stores first 3 fields as little-endian, rest as big-endian
    return (b[3::-1] + b[5:3:-1] + b[7:5:-1] + b[8:16])


def make_protective_mbr():
    """Create a protective MBR for GPT disk."""
    mbr = bytearray(512)

    # Partition entry 1 at offset 446 (protective GPT partition)
    entry = bytearray(16)
    entry[0] = 0x00           # Boot indicator (not active)
    entry[1:4] = b'\x00\x02\x00'  # CHS of first sector
    entry[4] = 0xEE           # Partition type: GPT protective
    entry[5:8] = b'\xFF\xFF\xFF'  # CHS of last sector
    struct.pack_into('<I', entry, 8, 1)  # LBA of first sector
    sectors = min(TOTAL_SECTORS - 1, 0xFFFFFFFF)
    struct.pack_into('<I', entry, 12, sectors)
    mbr[446:462] = entry

    # Boot signature
    mbr[510] = 0x55
    mbr[511] = 0xAA
    return bytes(mbr)


def make_gpt_entry(type_guid, unique_guid, start_lba, end_lba, name):
    """Create a single GPT partition entry (128 bytes)."""
    entry = bytearray(GPT_ENTRY_SIZE)
    entry[0:16] = guid_to_mixed_endian(type_guid)
    entry[16:32] = guid_to_mixed_endian(unique_guid)
    struct.pack_into('<Q', entry, 32, start_lba)
    struct.pack_into('<Q', entry, 40, end_lba)
    struct.pack_into('<Q', entry, 48, 0)  # Attributes

    # Name (UTF-16LE, max 36 chars)
    name_bytes = name.encode('utf-16-le')[:72]
    entry[56:56+len(name_bytes)] = name_bytes
    return bytes(entry)


def make_gpt_header(disk_guid, entries_crc32, is_backup=False):
    """Create a GPT header (92 bytes, padded to 512)."""
    if is_backup:
        my_lba = TOTAL_SECTORS - 1
        alt_lba = 1
        entries_lba = TOTAL_SECTORS - 1 - GPT_ENTRIES_SECTORS
    else:
        my_lba = 1
        alt_lba = TOTAL_SECTORS - 1
        entries_lba = GPT_ENTRIES_LBA

    last_usable = TOTAL_SECTORS - 1 - GPT_ENTRIES_SECTORS - 1

    hdr = bytearray(92)
    hdr[0:8] = b'EFI PART'                            # Signature
    struct.pack_into('<I', hdr, 8, 0x00010000)         # Revision 1.0
    struct.pack_into('<I', hdr, 12, 92)                # Header size
    struct.pack_into('<I', hdr, 16, 0)                 # CRC32 (filled later)
    struct.pack_into('<I', hdr, 20, 0)                 # Reserved
    struct.pack_into('<Q', hdr, 24, my_lba)            # My LBA
    struct.pack_into('<Q', hdr, 32, alt_lba)           # Alternate LBA
    struct.pack_into('<Q', hdr, 40, ESP_START_LBA - 1 if not is_backup else ESP_START_LBA - 1)
    # Actually, first usable LBA is after GPT entries
    struct.pack_into('<Q', hdr, 40, GPT_ENTRIES_LBA + GPT_ENTRIES_SECTORS)  # First usable
    struct.pack_into('<Q', hdr, 48, last_usable)       # Last usable
    hdr[56:72] = guid_to_mixed_endian(disk_guid)       # Disk GUID
    struct.pack_into('<Q', hdr, 72, entries_lba)       # Partition entries LBA
    struct.pack_into('<I', hdr, 80, GPT_ENTRIES_COUNT) # Number of entries
    struct.pack_into('<I', hdr, 84, GPT_ENTRY_SIZE)    # Entry size
    struct.pack_into('<I', hdr, 88, entries_crc32)     # Entries CRC32

    # Calculate header CRC32
    hdr_crc = zlib.crc32(bytes(hdr)) & 0xFFFFFFFF
    struct.pack_into('<I', hdr, 16, hdr_crc)

    # Pad to sector
    sector = bytearray(SECTOR_SIZE)
    sector[:92] = hdr
    return bytes(sector)


# ═══════════════════════════════════════════════════════════
# Minimal FAT16 Filesystem (UEFI ESP supports FAT12/16/32)
# ═══════════════════════════════════════════════════════════

class FAT16Builder:
    """Builds a minimal FAT16 filesystem image for the ESP."""

    def __init__(self, total_sectors):
        self.total_sectors = total_sectors
        self.sector_size = 512
        self.sectors_per_cluster = 4     # 2KB clusters
        self.reserved_sectors = 1        # Just the BPB sector
        self.num_fats = 2
        self.root_entry_count = 512      # Standard root dir entries
        self.root_dir_sectors = (self.root_entry_count * 32 + 511) // 512  # 32 sectors

        # Calculate FAT size (each entry = 2 bytes for FAT16)
        data_sectors = total_sectors - self.reserved_sectors - self.root_dir_sectors
        entries_per_fat_sector = self.sector_size // 2  # 256 entries/sector
        total_data_clusters = data_sectors // self.sectors_per_cluster
        self.fat_sectors = (total_data_clusters * 2 + self.sector_size - 1) // self.sector_size
        # Recalculate with FAT overhead
        overhead = self.reserved_sectors + self.num_fats * self.fat_sectors + self.root_dir_sectors
        self.total_clusters = (total_sectors - overhead) // self.sectors_per_cluster
        self.data_start_sector = overhead

        # FAT table (16-bit entries, clusters start at 2)
        self.fat = [0] * (self.total_clusters + 2)
        self.fat[0] = 0xFFF8  # Media descriptor
        self.fat[1] = 0xFFFF  # End of chain marker
        self.next_free_cluster = 2

        # Cluster data storage
        self.cluster_data = {}

        # Root directory entries (stored separately, not in a cluster)
        self.root_entries = []

    def _alloc_cluster(self):
        c = self.next_free_cluster
        if c >= self.total_clusters + 2:
            raise RuntimeError("FAT16: Out of clusters")
        self.next_free_cluster += 1
        return c

    def _alloc_chain(self, num_clusters):
        if num_clusters == 0:
            return 0
        first = self._alloc_cluster()
        prev = first
        for _ in range(num_clusters - 1):
            c = self._alloc_cluster()
            self.fat[prev] = c
            prev = c
        self.fat[prev] = 0xFFFF  # End of chain
        return first

    def _cluster_byte_offset(self, cluster):
        return (self.data_start_sector + (cluster - 2) * self.sectors_per_cluster) * self.sector_size

    def _add_dir_entry(self, parent_cluster, entry_bytes):
        """Add a 32-byte directory entry to a cluster-based directory."""
        cluster_size = self.sectors_per_cluster * self.sector_size
        if parent_cluster not in self.cluster_data:
            self.cluster_data[parent_cluster] = bytearray(cluster_size)
        dir_data = bytearray(self.cluster_data[parent_cluster])
        for i in range(0, len(dir_data), 32):
            if dir_data[i] == 0x00 or dir_data[i] == 0xE5:
                dir_data[i:i+32] = entry_bytes
                self.cluster_data[parent_cluster] = bytes(dir_data)
                return
        raise RuntimeError("Directory cluster full")

    def add_file(self, name_8_3, data, subdir_cluster=None):
        cluster_size = self.sectors_per_cluster * self.sector_size
        num_clusters = (len(data) + cluster_size - 1) // cluster_size if data else 0
        first_cluster = self._alloc_chain(num_clusters) if num_clusters > 0 else 0

        # Write data to clusters
        if data:
            cluster = first_cluster
            offset = 0
            while offset < len(data):
                chunk = data[offset:offset + cluster_size]
                self.cluster_data[cluster] = chunk
                offset += cluster_size
                if offset < len(data):
                    cluster = self.fat[cluster]

        entry = bytearray(32)
        entry[0:11] = name_8_3.encode('ascii')[:11].ljust(11)
        entry[11] = 0x20  # ATTR_ARCHIVE
        struct.pack_into('<H', entry, 20, (first_cluster >> 16) & 0xFFFF)
        struct.pack_into('<H', entry, 26, first_cluster & 0xFFFF)
        struct.pack_into('<I', entry, 28, len(data))

        if subdir_cluster is not None:
            self._add_dir_entry(subdir_cluster, entry)
        else:
            self.root_entries.append(bytes(entry))
        return first_cluster

    def add_directory(self, name_8_3, parent_cluster=None):
        cluster = self._alloc_cluster()
        self.fat[cluster] = 0xFFFF  # End of chain

        cluster_size = self.sectors_per_cluster * self.sector_size
        dir_data = bytearray(cluster_size)

        dot = bytearray(32)
        dot[0:11] = b'.          '
        dot[11] = 0x10
        struct.pack_into('<H', dot, 26, cluster & 0xFFFF)
        dir_data[0:32] = dot

        dotdot = bytearray(32)
        dotdot[0:11] = b'..         '
        dotdot[11] = 0x10
        # parent cluster 0 = root directory for FAT16
        dir_data[32:64] = dotdot

        self.cluster_data[cluster] = bytes(dir_data)

        entry = bytearray(32)
        entry[0:11] = name_8_3.encode('ascii')[:11].ljust(11)
        entry[11] = 0x10  # ATTR_DIRECTORY
        struct.pack_into('<H', entry, 26, cluster & 0xFFFF)

        if parent_cluster is not None:
            self._add_dir_entry(parent_cluster, entry)
        else:
            self.root_entries.append(bytes(entry))

        return cluster

    def build(self):
        image = bytearray(self.total_sectors * self.sector_size)

        # ── BPB (FAT16) ──
        bpb = bytearray(512)
        bpb[0:3] = b'\xEB\x3C\x90'        # Jump + NOP (FAT16 style)
        bpb[3:11] = b'AIOS    '            # OEM
        struct.pack_into('<H', bpb, 11, 512)                    # Bytes/sector
        bpb[13] = self.sectors_per_cluster                       # Sectors/cluster
        struct.pack_into('<H', bpb, 14, self.reserved_sectors)  # Reserved
        bpb[16] = self.num_fats                                  # FATs
        struct.pack_into('<H', bpb, 17, self.root_entry_count)  # Root entries
        if self.total_sectors < 65536:
            struct.pack_into('<H', bpb, 19, self.total_sectors)  # Total16
        else:
            struct.pack_into('<I', bpb, 32, self.total_sectors)  # Total32
        bpb[21] = 0xF8                                           # Media (fixed)
        struct.pack_into('<H', bpb, 22, self.fat_sectors)       # FATSz16
        struct.pack_into('<H', bpb, 24, 63)                     # Sectors/track
        struct.pack_into('<H', bpb, 26, 255)                    # Heads
        struct.pack_into('<I', bpb, 28, 0)                      # Hidden sectors
        # Extended boot record (FAT16)
        bpb[36] = 0x80                                           # Drive number
        bpb[38] = 0x29                                           # Extended boot sig
        struct.pack_into('<I', bpb, 39, 0x12345678)             # Volume serial
        bpb[43:54] = b'AIOS EFI   '                            # Volume label
        bpb[54:62] = b'FAT16   '                               # FS type hint
        bpb[510] = 0x55
        bpb[511] = 0xAA
        image[0:512] = bpb

        # ── FAT tables (16-bit entries) ──
        fat_bytes = bytearray(self.fat_sectors * self.sector_size)
        for i, val in enumerate(self.fat):
            off = i * 2
            if off + 2 <= len(fat_bytes):
                struct.pack_into('<H', fat_bytes, off, val & 0xFFFF)

        fat1_off = self.reserved_sectors * self.sector_size
        fat2_off = (self.reserved_sectors + self.fat_sectors) * self.sector_size
        image[fat1_off:fat1_off + len(fat_bytes)] = fat_bytes
        image[fat2_off:fat2_off + len(fat_bytes)] = fat_bytes

        # ── Root directory (fixed location after FATs) ──
        root_off = (self.reserved_sectors + self.num_fats * self.fat_sectors) * self.sector_size

        # Volume label entry
        vol = bytearray(32)
        vol[0:11] = b'AIOS EFI   '
        vol[11] = 0x08
        image[root_off:root_off+32] = vol

        entry_off = root_off + 32
        for entry in self.root_entries:
            image[entry_off:entry_off+32] = entry
            entry_off += 32

        # ── Cluster data ──
        for cluster_num, data in self.cluster_data.items():
            off = self._cluster_byte_offset(cluster_num)
            end = off + len(data)
            if end <= len(image):
                image[off:end] = data

        return bytes(image)


def build_disk_image(output_path, bootloader_path, kernel_path):
    """Build the complete GPT disk image."""
    print(f"Building UEFI disk image: {output_path}")

    # Read input files
    with open(bootloader_path, 'rb') as f:
        bootloader_data = f.read()
    print(f"  Bootloader: {bootloader_path} ({len(bootloader_data)} bytes)")

    with open(kernel_path, 'rb') as f:
        kernel_data = f.read()
    print(f"  Kernel: {kernel_path} ({len(kernel_data)} bytes)")

    # Create the full disk image
    disk = bytearray(DISK_SIZE)

    # ── 1. Protective MBR ──
    disk[0:512] = make_protective_mbr()

    # ── 2. Build FAT32 ESP ──
    fat32 = FAT16Builder(ESP_SECTORS)

    # Create EFI\BOOT directory structure
    efi_cluster = fat32.add_directory("EFI        ")
    boot_cluster = fat32.add_directory("BOOT       ", efi_cluster)

    # Add BOOTX64.EFI to \EFI\BOOT\
    fat32.add_file("BOOTX64 EFI", bootloader_data, boot_cluster)

    # Add kernel.elf to root of ESP
    fat32.add_file("KERNEL  ELF", kernel_data)

    esp_image = fat32.build()
    esp_offset = ESP_START_LBA * SECTOR_SIZE
    disk[esp_offset:esp_offset + len(esp_image)] = esp_image

    print(f"  ESP: LBA {ESP_START_LBA}-{ESP_END_LBA} ({ESP_SECTORS * 512 // 1024}KB FAT32)")
    print(f"  ChaosFS: LBA {CHAOSFS_START_LBA}+ ({(TOTAL_SECTORS - CHAOSFS_START_LBA) * 512 // 1024 // 1024}MB)")

    # ── 3. GPT Partition Entries ──
    disk_guid = uuid.uuid4()
    esp_guid = uuid.uuid4()
    chaosfs_guid = uuid.uuid4()

    # ChaosFS partition ends before backup GPT
    chaosfs_end_lba = TOTAL_SECTORS - 1 - GPT_ENTRIES_SECTORS - 1

    entries = bytearray(GPT_ENTRIES_COUNT * GPT_ENTRY_SIZE)

    # Entry 0: EFI System Partition
    entries[0:128] = make_gpt_entry(
        EFI_SYSTEM_PARTITION_GUID, esp_guid,
        ESP_START_LBA, ESP_END_LBA, "EFI System")

    # Entry 1: ChaosFS data partition
    entries[128:256] = make_gpt_entry(
        LINUX_FILESYSTEM_GUID, chaosfs_guid,
        CHAOSFS_START_LBA, chaosfs_end_lba, "ChaosFS")

    entries_crc = zlib.crc32(bytes(entries)) & 0xFFFFFFFF

    # Write primary partition entries (LBA 2-33)
    entries_offset = GPT_ENTRIES_LBA * SECTOR_SIZE
    disk[entries_offset:entries_offset + len(entries)] = entries

    # Write backup partition entries (before last sector)
    backup_entries_lba = TOTAL_SECTORS - 1 - GPT_ENTRIES_SECTORS
    backup_entries_offset = backup_entries_lba * SECTOR_SIZE
    disk[backup_entries_offset:backup_entries_offset + len(entries)] = entries

    # ── 4. GPT Headers ──
    primary_hdr = make_gpt_header(disk_guid, entries_crc, is_backup=False)
    disk[GPT_HEADER_LBA * SECTOR_SIZE:(GPT_HEADER_LBA + 1) * SECTOR_SIZE] = primary_hdr

    backup_hdr = make_gpt_header(disk_guid, entries_crc, is_backup=True)
    disk[(TOTAL_SECTORS - 1) * SECTOR_SIZE:TOTAL_SECTORS * SECTOR_SIZE] = backup_hdr

    # ── 5. Write disk image ──
    with open(output_path, 'wb') as f:
        f.write(bytes(disk))

    print(f"  Disk image written: {DISK_SIZE // 1024 // 1024}MB")
    return CHAOSFS_START_LBA


if __name__ == '__main__':
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <output.img> <BOOTX64.EFI> <kernel.elf>")
        sys.exit(1)

    chaosfs_lba = build_disk_image(sys.argv[1], sys.argv[2], sys.argv[3])
    print(f"  ChaosFS LBA offset: {chaosfs_lba}")
