# ChaosFS v2 — Filesystem Specification (Revised)
## AIOS v2 Native Filesystem

---

## Preface: Lessons From v1

ChaosFS v1 wasn't designed — it was accumulated. Features were bolted on as needed, contiguous block allocation meant fragmentation was inevitable, there was no coherent on-disk layout contract, and the whole thing would have become unusable on a real workload inside a few hours of use.

**v2 rules (same as the rest of AIOS v2):**
1. **Design for the final system.** Lua scripts, config files, textures, audio samples, save states, Doom maps — all of these live on this filesystem. It must handle them without degrading.
2. **Every on-disk structure is a documented contract.** Magic numbers, field offsets, sizes — all explicit, all asserted in code.
3. **No silent failures.** Every operation returns a real error code.
4. **Extent-based allocation from day one.** The v1 contiguous allocation death spiral does not happen in v2.
5. **Simple enough to implement correctly.** No journaling in v2 — but designed so journaling can be added in v2.1 without breaking the format.

---

## Design Goals

| Goal | Decision |
|------|----------|
| No fragmentation death spiral | Extent-based allocation — files can span non-contiguous blocks |
| Works on real hardware | ATA/IDE DMA, 28-bit LBA, 512-byte sectors |
| Fast enough for Lua's alloc pattern | 4KB blocks match page size, bitmap allocation is O(n/32) with next-fit |
| Mountable on host for dev | Eventually: write a FUSE driver. For now: format tool and dump utility |
| Self-contained | No external dependencies. Pure C, uses only our own ATA driver and heap |
| Recoverable | Clean unmount flag, fsck-able structure, backup superblock |
| Extensible | Reserved fields, version field, journal block pointer (unused but present) |

---

## Disk Layout

ChaosFS occupies a contiguous region of the disk starting at a fixed LBA offset.

**Recommended placement:** LBA 2048 (1MB offset from disk start). This leaves room for:
- MBR + Stage 1 (sector 0)
- Stage 2 (sectors 1-16)
- Kernel ELF (sectors 17 - ~1200 for a 600KB kernel)
- Padding to 1MB boundary

The filesystem start LBA is stored in the superblock and passed to `chaos_mount()`. It is NOT hardcoded in the driver — the driver is position-independent on disk.

**Who calls chaos_format():** The AIOS build system creates a pre-formatted disk image using a host-side tool that calls `chaos_format()`. The kernel does NOT auto-format on first boot — if `chaos_mount()` fails, it logs a fatal error and halts. Formatting is a deliberate offline operation, not a runtime fallback.

### Block Size

**4096 bytes per block (8 x 512-byte sectors). Fixed. Not configurable.**

This is a design contract. It matches the PMM page size, simplifies all offset math, and is the right size for every workload we have.

### On-Disk Region Layout

```
[FS Block 0]     Superblock (primary)
[FS Block 1]     Superblock (backup — written on every clean unmount)
[FS Block 2]     Block allocation bitmap — block 0 (covers blocks 0..32767)
[FS Block 3..N]  Block allocation bitmap — continued as needed
[FS Block N+1]   Inode table — first block (32 inodes per block)
[FS Block N+2..M] Inode table — continued
[FS Block M+1..]  Data blocks
```

**All metadata blocks (superblock, bitmap, inode table) are pre-marked as used in the bitmap.** The bitmap covers every block including itself. Allocation code never needs special-case logic for metadata.

**The superblock stores the exact starting block index of the bitmap, inode table, and data region.** These are computed at format time and never change.

---

## On-Disk Structures

All structures are packed (`__attribute__((packed))`). All multi-byte integers are little-endian (x86 native).

### Superblock

**Located at block 0. Backup copy at block 1.**

```c
#define CHAOS_MAGIC         0x43484653  /* 'CHFS' */
#define CHAOS_VERSION       2
#define CHAOS_BLOCK_SIZE    4096

struct chaos_superblock {
    /* Identity */
    uint32_t magic;             /* Must be CHAOS_MAGIC */
    uint32_t version;           /* Currently 2 */
    char     fs_name[16];       /* Human-readable label, null-padded */

    /* Geometry — set at format, never change */
    uint32_t block_size;        /* Always 4096. Assert on mount. */
    uint32_t total_blocks;      /* Total blocks in filesystem */
    uint32_t bitmap_start;      /* First block of bitmap region */
    uint32_t bitmap_blocks;     /* Number of blocks in bitmap */
    uint32_t inode_table_start; /* First block of inode table */
    uint32_t inode_table_blocks;/* Number of blocks in inode table */
    uint32_t data_start;        /* First usable data block */

    /* Dynamic counts — updated on every alloc/free */
    uint32_t total_inodes;      /* Total inodes (set at format) */
    uint32_t free_blocks;       /* Current free block count */
    uint32_t free_inodes;       /* Current free inode count */

    /* State */
    uint8_t  clean_unmount;     /* 1 = cleanly unmounted, 0 = dirty (needs fsck) */
    uint8_t  mounted;           /* 1 = currently mounted */
    uint16_t mount_count;       /* How many times mounted (informational) */

    /* Timestamps — seconds since last boot (monotonic, not wall clock).
     * NOT Unix timestamps. Using timer_get_ticks() / PIT_FREQUENCY.
     * Sufficient for ordering and last-modified tracking.
     * If RTC support is added later, a format version bump will upgrade this. */
    uint32_t created_time;
    uint32_t last_mounted_time;
    uint32_t last_fsck_time;    /* 0 = never */

    /* Future extensions */
    uint32_t journal_start;     /* 0 = no journal. Reserved for v2.1. */
    uint32_t journal_blocks;    /* 0 = no journal. Reserved for v2.1. */
    uint8_t  reserved[28];      /* Zero on format, ignored on mount */

    /* Integrity */
    uint32_t checksum;          /* CRC-32/ISO-HDLC (polynomial 0xEDB88320, reflected)
                                 * of all preceding bytes in this struct.
                                 * Same algorithm as zlib/Ethernet.
                                 * Computed on write, verified on mount.
                                 * Mismatch → try backup superblock. */
} __attribute__((packed));

_Static_assert(sizeof(struct chaos_superblock) <= 4096,
               "chaos_superblock exceeds one block!");
```

**On mount:**
1. Read block 0. Verify magic, version, block_size, checksum. If any fail, try block 1.
2. If block 1 also fails: return CHAOS_ERR_CORRUPT.
3. If block 1 succeeds: log warning "primary superblock corrupt, using backup", proceed.
4. Reject version != CHAOS_VERSION. Reject block_size != 4096.
5. If clean_unmount == 0: log warning, call chaos_fsck().
6. Set mounted = 1, increment mount_count, update last_mounted_time.
7. Write updated superblock to block 0 AND block 1.
8. Load bitmap into RAM cache.
9. Zero-initialise inode cache.

**On unmount:**
1. Flush all dirty inode cache entries to disk.
2. Set clean_unmount = 1, mounted = 0.
3. Write superblock to block 0 and block 1.

---

### Block Allocation Bitmap

One bit per block. 0 = free, 1 = used. Packed into 4096-byte bitmap blocks.

Each bitmap block covers 4096 × 8 = 32768 blocks. For a 256MB filesystem with 4KB blocks: 65536 total blocks → 2 bitmap blocks.

**Bitmap includes ALL blocks including metadata.** Blocks 0..data_start-1 are marked used at format and permanently used.

**The in-memory bitmap cache is authoritative for the duration of a mount session.** All allocation and free operations update the cache immediately. The cache is the source of truth — the on-disk copy is kept in sync by write-through: whenever the cache is modified, the affected 4KB bitmap block is written to disk before the function returns. This means the on-disk bitmap is never more than one word behind the cache at any point.

**Allocation strategy: next-fit with 32-bit word scan.**

```c
/* The bitmap is kept in memory during mount.
 * For 256MB filesystem: 65536 bits = 8KB. Trivial memory cost.
 * Allocated from kernel heap during chaos_mount(). Freed on chaos_unmount(). */
static uint32_t* bitmap_cache;       /* in-memory bitmap */
static uint32_t  bitmap_cache_words; /* total 32-bit words */
static uint32_t  next_alloc_hint;    /* word index for next-fit scan */

/* Null block sentinel — returned by chaos_alloc_block() on failure.
 * Block 0 is always the superblock (always used) so 0 is a safe invalid value.
 * ALL callers of chaos_alloc_block() MUST check for CHAOS_BLOCK_NULL. */
#define CHAOS_BLOCK_NULL  0
```

**chaos_alloc_block():**
1. Scan from next_alloc_hint forward, one uint32_t at a time
2. For each word that isn't 0xFFFFFFFF (has a free bit), find first zero bit (`__builtin_ctz(~word)`)
3. Compute block index = word_index * 32 + bit_index
4. Verify block_index >= data_start. If not (somehow scanning metadata region), continue scanning — this should never happen since metadata bits are always set, but assert in debug builds.
5. Set bit in cache, write updated word to disk (write-through — write the 4KB bitmap block containing this word)
6. Decrement free_blocks in superblock cache (superblock written separately by caller or on unmount — free_blocks is informational, see write ordering)
7. Update next_alloc_hint = current word_index
8. Return block_index
9. If scan wraps fully around with no free bit found: return CHAOS_BLOCK_NULL

**chaos_free_block(uint32_t block_index):**
1. Assert block_index >= data_start (freeing metadata blocks is a bug)
2. Assert bit is currently set (double-free is a bug)
3. Clear bit in cache, write updated bitmap word to disk
4. Increment free_blocks
5. If block_index / 32 < next_alloc_hint: update next_alloc_hint = block_index / 32 (freed block is before hint, reset hint to find it sooner)

---

### Inode

**Fixed size: 128 bytes. 32 inodes per 4KB block.**

```c
#define CHAOS_INODE_MAGIC        0xC4A0
#define CHAOS_MAX_INLINE_EXTENTS 6
#define CHAOS_INODE_SIZE         128

#define CHAOS_TYPE_FILE          0x1000
#define CHAOS_TYPE_DIR           0x2000
#define CHAOS_TYPE_SYMLINK       0x3000  /* reserved, not implemented in v2 */
#define CHAOS_TYPE_MASK          0xF000

struct chaos_extent {
    uint32_t start_block;   /* first block of this extent (CHAOS_BLOCK_NULL = unused) */
    uint32_t block_count;
} __attribute__((packed));  /* 8 bytes */

struct chaos_inode {
    uint16_t magic;         /* CHAOS_INODE_MAGIC — slot is live if this matches */
    uint16_t mode;          /* type (high 4 bits) + permissions (low 12 bits) */
    uint32_t link_count;    /* number of directory entries pointing to this inode */
    uint32_t open_count;    /* number of currently open file descriptors.
                             * NOT stored on disk (zeroed on load).
                             * Used to defer unlink of open files. */
    bool     unlink_pending;/* set by chaos_unlink() if open_count > 0.
                             * When last fd closes and unlink_pending is true,
                             * the inode and blocks are freed at that point.
                             * NOT stored on disk (cleared on load). */
    uint8_t  pad0[3];
    uint64_t size;          /* file size in bytes */
    uint32_t created_time;
    uint32_t modified_time;
    uint32_t accessed_time;
    uint32_t flags;         /* reserved, zero */

    uint8_t  extent_count;
    uint8_t  has_indirect;  /* 1 = indirect_block is valid */
    uint16_t reserved;
    struct chaos_extent extents[CHAOS_MAX_INLINE_EXTENTS];  /* 6 × 8 = 48 bytes */
    uint32_t indirect_block;

    uint8_t  pad1[2];
} __attribute__((packed));

_Static_assert(sizeof(struct chaos_inode) == 128, "chaos_inode must be 128 bytes!");
_Static_assert(sizeof(struct chaos_extent) == 8,  "chaos_extent must be 8 bytes!");
```

**Important: `open_count` and `unlink_pending` are runtime-only fields.** They are NOT persisted to disk. On load from disk, `open_count` is always set to 0 and `unlink_pending` is always set to false. They exist only in the inode cache.

**Inode numbering:** 1-based. Inode 0 is CHAOS_INODE_NULL — always invalid. Inode 1 is always the root directory. Fixed invariant, asserted on mount.

```c
#define CHAOS_INODE_NULL  0  /* invalid inode number — like CHAOS_BLOCK_NULL */
```

**Inode table slot 0 is permanently reserved and never allocated.** It corresponds to inode number 0 (CHAOS_INODE_NULL). The first 128 bytes of the first inode table block are zeroed at format and never written. This wastes 128 bytes. That is acceptable.

**Inode allocation:** scan inode table for first slot where `magic != CHAOS_INODE_MAGIC`. A freed inode has magic zeroed — so this single check is the definitive "slot is free" test. Do NOT check `link_count` as a free indicator — a live inode can legally have `link_count == 0` if it was unlinked while open. The magic field is the only reliable slot-free indicator.

**Inode free:** zero the entire 128-byte inode struct, write back to disk. This clears the magic, marking the slot as available.

### Link Count Rules

Link counts must be maintained correctly for fsck to work.

**For files:**
- `link_count` = number of directory entries (across all directories) that reference this inode
- Starts at 1 when created (the entry in the parent directory)
- Incremented by hard links (not in v2 — no hard link API — but link_count exists for fsck purposes)
- Decremented by `chaos_unlink()`
- When `link_count` reaches 0 AND `open_count` == 0: inode and blocks are freed

**For directories:**
- `link_count` starts at 2 when created: one for the entry in the parent directory, one for the directory's own `.` entry
- Each subdirectory created inside this directory increments this directory's `link_count` by 1 (because the subdirectory's `..` entry points here)
- Each subdirectory removed decrements this directory's `link_count` by 1
- Minimum link_count for any directory: 2 (empty directory with no subdirectories)
- Root directory (inode 1): link_count starts at 2 (`/` itself + `/` `.`), increments for each direct subdirectory

**fsck cross-check:** walk all directory entries, count references per inode, compare to stored link_count. Repair if mismatched.

### Inode Cache

**16 entries, LRU eviction.**

```c
#define INODE_CACHE_SIZE 16

struct inode_cache_entry {
    uint32_t inode_num;   /* 0 = empty slot */
    bool     dirty;       /* modified since load — MUST be written before eviction */
    uint32_t last_used;   /* tick count at last access, for LRU */
    struct chaos_inode inode;
};

static struct inode_cache_entry inode_cache[INODE_CACHE_SIZE];
```

**Dirty inode eviction contract:** before evicting an entry where `dirty == true`, write the inode to disk. This is mandatory. Silent eviction of dirty inodes is data loss. No exceptions.

**Cache invalidation on fsck:** chaos_fsck() clears the entire inode cache before starting its walk and after completing repairs. This prevents the cache from serving stale data after fsck modifies on-disk inodes.

### Indirect Extent Block

When a file needs more than 6 extents, `has_indirect = 1` and `indirect_block` points to a 4KB data block used entirely for additional extents.

```
One indirect block = 4096 / 8 = 512 additional extents
Total extents per file = 6 (inline) + 512 (indirect) = 518
```

**Maximum file size via extent limit:** In the worst case (maximum fragmentation — every extent is exactly 1 block), 518 extents × 4KB = ~2MB. In typical use (extents are many blocks each), the practical size limit is the disk size. A file written in one sequential session will use 1 extent regardless of size. The 518-extent limit is a fragmentation ceiling, not a size ceiling.

**If a write would require a 519th extent:** `chaos_write()` returns `CHAOS_ERR_NO_SPACE` even if free disk blocks are available. This is logged with a distinct message so it's diagnosable. In practice, reaching this limit requires writing to the same file hundreds of times in non-contiguous sessions with heavy interleaving with other file writes between each session — essentially impossible in normal AIOS use.

---

### Directory Entry

```c
#define CHAOS_DIRENT_SIZE   64
#define CHAOS_MAX_FILENAME  53      /* 53 chars + 1 null terminator = 54 bytes in field */
#define CHAOS_DT_FILE       1
#define CHAOS_DT_DIR        2

struct chaos_dirent {
    uint32_t inode;             /* CHAOS_INODE_NULL (0) = unused slot */
    uint8_t  type;              /* CHAOS_DT_FILE or CHAOS_DT_DIR */
    uint8_t  name_len;          /* strlen(name), not including null */
    char     name[54];          /* null-terminated, null-padded to 54 bytes */
    uint8_t  reserved[2];
} __attribute__((packed));

_Static_assert(sizeof(struct chaos_dirent) == 64, "chaos_dirent must be 64 bytes!");
```

**64 directory entries per 4KB block.**

**Special entries:** every directory block starts with `.` (self) and `..` (parent) as the first two entries. For root, both point to inode 1. These are written at mkdir time and never deleted or moved.

**Entry deletion:** set `inode = CHAOS_INODE_NULL`. Slot becomes available for reuse. Directories are not compacted.

**Entry creation:** scan directory blocks for first slot where `inode == CHAOS_INODE_NULL`. If no free slot in existing blocks, allocate a new block from the bitmap, add it to the directory's extents, initialise all 64 slots to inode=0, use slot 0 of the new block.

---

### Directory Handle Table

Separate from the fd table. Used by `chaos_opendir()` / `chaos_readdir()` / `chaos_closedir()`.

```c
#define CHAOS_MAX_DIR_HANDLES  8

struct chaos_dir_handle {
    bool     in_use;
    uint32_t inode_num;       /* directory inode */
    uint32_t block_idx;       /* which extent block we're currently reading */
    uint32_t slot_idx;        /* which slot within that block (0-63) */
};

static struct chaos_dir_handle dir_handle_table[CHAOS_MAX_DIR_HANDLES];
```

`chaos_opendir()` scans for first `in_use == false` entry. Returns handle index (0-7) or `CHAOS_ERR_NO_FD` if all 8 are in use.

`chaos_readdir()` advances through slots, skipping `inode == 0` entries, advancing through blocks as needed. Returns `CHAOS_OK` with the next live entry, or `CHAOS_ERR_NOT_FOUND` when the directory is exhausted.

`chaos_closedir()` sets `in_use = false`. Returns `CHAOS_OK` or `CHAOS_ERR_BAD_FD` on invalid handle.

---

## File Descriptor Table

```c
#define CHAOS_MAX_FD    16

typedef enum {
    FD_FLAG_READ    = 0x01,
    FD_FLAG_WRITE   = 0x02,
    FD_FLAG_APPEND  = 0x04,
} fd_flags_t;

struct chaos_fd {
    bool     in_use;
    uint32_t inode_num;
    uint64_t position;
    uint8_t  flags;
};

static struct chaos_fd fd_table[CHAOS_MAX_FD];
```

**open() flags:**
```c
#define CHAOS_O_RDONLY   0x01
#define CHAOS_O_WRONLY   0x02
#define CHAOS_O_RDWR     0x03
#define CHAOS_O_CREAT    0x04
#define CHAOS_O_TRUNC    0x08
#define CHAOS_O_APPEND   0x10
```

---

## Unlink-While-Open Contract

ChaosFS supports POSIX-style unlink-while-open. A file that is unlinked while open continues to be accessible via existing file descriptors until the last fd is closed.

**chaos_unlink() behaviour:**
1. Resolve path to inode
2. Remove the directory entry (set slot inode = CHAOS_INODE_NULL)
3. Decrement `link_count`
4. If `link_count == 0` AND `open_count == 0`: free inode and blocks immediately
5. If `link_count == 0` AND `open_count > 0`: set `unlink_pending = true`. Do NOT free yet. The file data remains accessible via open fds.

**chaos_close() behaviour (additional step):**
After the normal close logic, check: if `open_count` drops to 0 AND `unlink_pending == true`, free the inode and all its blocks at that point.

**open_count is maintained by the fd table operations:**
- `chaos_open()` increments `open_count` on the inode (via cache)
- `chaos_close()` decrements `open_count`, then checks `unlink_pending`

---

## Path Resolution

```c
/* Resolve absolute path to inode number.
 * Returns inode number (>= 1) on success, CHAOS_INODE_NULL on failure. */
uint32_t chaos_resolve_path(const char* path);
```

**Constraints:**
- Must start with '/'
- Maximum length: 512 characters
- Maximum depth: 16 components
- No symlink traversal (CHAOS_TYPE_SYMLINK is reserved, unimplemented)
- `.` and `..` in path strings are NOT resolved specially by chaos_resolve_path — they are treated as literal directory names. The shell is responsible for canonicalising paths before passing them to filesystem functions.

**Algorithm:** start at inode 1, tokenise by '/', scan directory entries for each token, descend or return.

---

## chaos_rename Contract

**v2 scope: same-directory rename only.**

`chaos_rename(old_path, new_path)` requires that old_path and new_path share the same parent directory. If they do not, `chaos_rename()` returns `CHAOS_ERR_INVALID`. Cross-directory move is not supported in v2.

**Same-directory rename steps:**
1. Resolve old_path to inode. If not found: CHAOS_ERR_NOT_FOUND.
2. Verify new_path parent == old_path parent (same directory inode). If not: CHAOS_ERR_INVALID.
3. Extract new_name = last component of new_path.
4. If new_name == old_name: return CHAOS_OK (no-op).
5. If new_name already exists in the directory:
   - If it's a file: unlink the existing file (full unlink semantics including unlink-while-open), then proceed
   - If it's a directory: return CHAOS_ERR_NOT_EMPTY (refuse to replace a directory with rename)
6. Find the old directory entry, update its name field to new_name, update name_len. Write the directory block back.
7. Return CHAOS_OK.

---

## Write Ordering Contract

Without journaling, crash safety depends on writing things in the correct order. **This section is a binding implementation contract, not a suggestion.**

### Rule 1: Data before metadata

When extending a file:
1. Allocate block from bitmap (update in-memory bitmap cache + write-through bitmap word to disk)
2. Write the data to the new block
3. Update the inode's extent list to include the new block
4. Write the inode back to disk (via inode cache write-through)

**Never update the inode to reference a block before that block's data has been written.** An inode pointing to an unwritten block means the file contains garbage if we crash between steps 3 and 4. The correct crash failure mode is: block is allocated and written but inode doesn't reference it yet — fsck finds an allocated-but-unreferenced block and frees it. Data loss, but no corruption and no garbage reads.

### Rule 2: Inode before directory entry

When creating a new file:
1. Allocate inode, initialise it fully, write it to disk
2. Write the data blocks
3. Update the inode with extent info, write inode again
4. Write the directory entry pointing to the new inode

**Never write a directory entry pointing to an inode before the inode is fully written.** Correct crash failure mode: inode exists on disk but no directory entry — fsck finds the orphaned inode and frees it.

### Rule 3: Superblock free counts are informational

`free_blocks` and `free_inodes` in the superblock are updated in memory immediately but are written to disk lazily (on `chaos_sync()` or unmount). They are NEVER used as the sole authoritative source for free space. Before reporting "disk full", the implementation must verify against the actual bitmap. fsck can always recompute free counts from scratch.

### Rule 4: In-memory bitmap is authoritative

The bitmap cache is always up to date. The on-disk bitmap is kept in sync by write-through (the affected 4KB bitmap block is written to disk before chaos_alloc_block() or chaos_free_block() returns). On crash, at most one bitmap write was in flight. fsck recomputes the bitmap by walking all inodes and corrects any stale bits.

---

## Seek-Past-EOF Write Behaviour

If `chaos_seek()` positions the fd beyond the current file size, the next `chaos_write()` will:
1. Allocate blocks to cover the gap between the old EOF and the new write position
2. Zero-fill those gap blocks on disk
3. Continue with the actual write at the seeked position

This is explicit allocation, not sparse files. ChaosFS v2 does not support sparse files — every byte between offset 0 and the file's size has allocated, readable blocks. This simplifies everything: no "does this block exist?" logic in reads, no special fsck handling for holes.

The zero-fill is done block by block. If the gap is large (e.g. seek to 100MB in an empty file) this will be slow and will allocate a lot of disk space. This is intentional and expected — callers should not seek past EOF carelessly.

---

## Block I/O Layer

```c
/* All disk access goes through this layer. Never call ATA functions directly
 * from filesystem logic. */

/* CHAOS_BLOCK_NULL (0) is never a valid argument to these functions.
 * Passing block_idx == 0 to chaos_block_write() is a bug — assert fires. */
int chaos_block_read(uint32_t block_idx, void* buffer);
int chaos_block_write(uint32_t block_idx, const void* buffer);
```

LBA translation: `lba = fs_lba_start + (block_idx * 8)`  (8 sectors per 4KB block)

**Block cache (512 entries, 2MB RAM, LRU, write-through).** The cache sits in this layer — `chaos_block_read()` checks cache before disk, `chaos_block_write()` writes through to disk and updates cache. Invalidation happens automatically in `chaos_free_block()`. The bitmap and inode caches remain separate (explicit, higher-level). Cache stats are exposed via `aios.os.cache_stats()` in Lua.

---

## Error Codes

```c
typedef enum {
    CHAOS_OK              =  0,
    CHAOS_ERR_NOT_FOUND   = -1,
    CHAOS_ERR_NOT_DIR     = -2,
    CHAOS_ERR_NOT_FILE    = -3,
    CHAOS_ERR_EXISTS      = -4,
    CHAOS_ERR_NO_SPACE    = -5,   /* disk full, OR extent limit (519th extent) reached */
    CHAOS_ERR_NO_INODES   = -6,
    CHAOS_ERR_NO_FD       = -7,   /* fd table OR dir handle table full */
    CHAOS_ERR_CORRUPT     = -8,
    CHAOS_ERR_IO          = -9,
    CHAOS_ERR_INVALID     = -10,  /* bad argument, or cross-directory rename */
    CHAOS_ERR_NOT_EMPTY   = -11,
    CHAOS_ERR_NOT_MOUNTED = -12,
    CHAOS_ERR_TOO_LONG    = -13,
    CHAOS_ERR_BAD_FD      = -14,
    CHAOS_ERR_READ_ONLY   = -15,
} chaos_err_t;
```

---

## Public API

```c
/* ── Lifecycle ──────────────────────────────────────────────── */

int  chaos_format(uint32_t lba_start, uint32_t lba_count, const char* label);
int  chaos_mount(uint32_t lba_start);
void chaos_unmount(void);
int  chaos_fsck(void);     /* returns error count; 0 = clean */
void chaos_sync(void);     /* flush dirty inode cache entries to disk */


/* ── File operations ────────────────────────────────────────── */

int     chaos_open(const char* path, int flags);
int     chaos_close(int fd);
int     chaos_read(int fd, void* buf, uint32_t len);
int     chaos_write(int fd, const void* buf, uint32_t len);
int64_t chaos_seek(int fd, int64_t offset, int whence);

#define CHAOS_SEEK_SET 0
#define CHAOS_SEEK_CUR 1
#define CHAOS_SEEK_END 2

int chaos_truncate(int fd, uint64_t size);
int chaos_unlink(const char* path);

/* Same-directory rename only. Returns CHAOS_ERR_INVALID if paths
 * have different parent directories. */
int chaos_rename(const char* old_path, const char* new_path);


/* ── Stat ───────────────────────────────────────────────────── */

struct chaos_stat {
    uint32_t inode;
    uint16_t mode;
    uint64_t size;
    uint32_t block_count;
    uint32_t created_time;
    uint32_t modified_time;
    uint32_t accessed_time;
};

int chaos_stat(const char* path, struct chaos_stat* st);
int chaos_fstat(int fd, struct chaos_stat* st);


/* ── Directory operations ───────────────────────────────────── */

int chaos_mkdir(const char* path);
int chaos_rmdir(const char* path);   /* CHAOS_ERR_NOT_EMPTY if not empty */
int chaos_opendir(const char* path);
int chaos_readdir(int handle, struct chaos_dirent* out);
int chaos_closedir(int handle);      /* returns CHAOS_OK or CHAOS_ERR_BAD_FD */


/* ── Utility ────────────────────────────────────────────────── */

uint32_t    chaos_free_blocks(void);
uint32_t    chaos_free_inodes(void);
uint32_t    chaos_total_blocks(void);
const char* chaos_label(void);
```

---

## On-Disk Format — chaos_format() Steps

**Minimum filesystem size:** The minimum is derived, not arbitrary.

```
minimum_blocks = 2               (superblocks)
               + 1               (at least one bitmap block)
               + 1               (at least one inode table block = 32 inodes)
               + 1               (root directory data block)
             = 5 blocks minimum
```

In practice, impose a floor of 64 blocks (256KB) to leave room for actual files. Assert at format time: `if (total_blocks < 64) return CHAOS_ERR_INVALID`.

**Format steps:**
1. Validate lba_count × 512 / 4096 >= 64 blocks
2. Compute layout:
   - `total_blocks = (lba_count * 512) / 4096`
   - `bitmap_blocks = (total_blocks + 32767) / 32768`
   - `inode_count = total_blocks / 4`  (tunable ratio; one inode per 4 data blocks)
   - `inode_table_blocks = (inode_count + 31) / 32`
   - `data_start = 2 + bitmap_blocks + inode_table_blocks`
3. Zero blocks 0..data_start-1
4. Build and write superblock to block 0 and block 1 (compute CRC-32 last)
5. Build bitmap: all zeros, then set bits for blocks 0..data_start-1 as used. Write bitmap blocks.
6. Zero inode table blocks (all slots have magic=0, i.e. unformatted)
7. Create root directory (inode 1):
   - Write inode 1: magic=CHAOS_INODE_MAGIC, mode=CHAOS_TYPE_DIR|0755, link_count=2, size=0, extent_count=0
   - Allocate one data block for root directory entries
   - Write . and .. entries in that block (both inode=1)
   - Update inode 1 with extent pointing to that block, size=4096
   - Write inode 1 back
8. Update superblock: free_blocks and free_inodes, write again to block 0 and block 1
9. Verify: read back superblock, check magic and CRC-32

---

## fsck Behaviour

`chaos_fsck()` is called automatically on mount when `clean_unmount == 0`. It can also be called as a shell command.

**What fsck checks and repairs:**
1. Superblock sanity (magic, version, block_size, CRC) — already done by mount, but fsck re-verifies
2. Bitmap vs inode cross-check: walk all inodes, collect all block indices referenced by live inodes, compare to bitmap. Any block referenced by an inode but marked free → mark used in bitmap (repair). Any block marked used but not referenced by any inode → mark free (repair, log as leaked block).
3. Link count cross-check: walk all directory entries, count references per inode, compare to inode.link_count. Mismatch → update inode.link_count to match actual count (repair).
4. Directory structure: verify every directory has valid . and .. entries. Verify .. of root points to root.
5. Extent validity: for each live inode, verify all extent block indices are >= data_start and < total_blocks. Invalid extent → log error, zero the extent (file is truncated to the last valid extent — data loss, but controlled).
6. Inode cache must be fully flushed (written to disk) before fsck starts, and fully invalidated after fsck completes.

**fsck returns:** number of errors found. 0 = clean. Errors that were repaired are counted. Errors that could not be repaired are also counted (and logged with more detail).

---

## Project Structure

```
kernel/
└── chaos/
    ├── chaos.h         # Public API
    ├── chaos.c         # Lifecycle, fd table, open/close/read/write/seek/truncate
    ├── chaos_alloc.c   # Block allocator (bitmap cache), inode allocator
    ├── chaos_dir.c     # mkdir, rmdir, opendir, readdir, closedir, resolve_path, rename
    ├── chaos_inode.c   # Inode read/write, inode cache (16 entries LRU, dirty write-back)
    ├── chaos_block.c   # Block I/O layer: chaos_block_read/write, LBA translation
    ├── chaos_format.c  # chaos_format()
    ├── chaos_fsck.c    # Consistency checker and repairer
    └── chaos_types.h   # ALL on-disk structs: superblock, inode, dirent, extent
```

`chaos_types.h` is the only header that defines on-disk structures. No ad-hoc serialisation elsewhere.

---

## Phase 4 Acceptance Tests

1. **Format + mount:** `chaos_format()` succeeds. Immediate mount: superblock valid, root inode exists (inode 1), `clean_unmount=1`, free counts correct. Static assert on all struct sizes passes compilation.
2. **Basic write/read:** create file, write "hello world", close. Open read-only, read back — matches exactly. `chaos_stat()` shows correct size.
3. **Large file:** write a 2MB file in 4KB chunks. Read back in full. Contents match. `extent_count > 1` in the inode — proves extent allocation works, not secretly contiguous.
4. **Fragmented write:** write 100 × 4KB files, delete every other one, write a 200KB file. Verify it succeeds (spans multiple extents across the holes) and reads back correctly. Verify `extent_count > 1`.
5. **Directory creation:** `mkdir("/scripts")`, `mkdir("/scripts/ai")`, create `/scripts/ai/test.lua`. Resolve path succeeds. `rmdir("/scripts")` returns `CHAOS_ERR_NOT_EMPTY`. `rmdir("/scripts/ai")` after unlinking test.lua returns `CHAOS_ERR_NOT_EMPTY` (dir still has . and ..). `chaos_unlink("/scripts/ai/test.lua")` then `chaos_rmdir("/scripts/ai")` succeeds. Verify link_count of `/scripts` decremented correctly.
6. **Directory listing:** create 5 files in `/`. `opendir("/")`, readdir loop — returns all 5 names plus `.` and `..`. No duplicates, no missing entries. `closedir()` returns `CHAOS_OK`.
7. **File deletion + inode reuse:** create file, unlink it. Free block count returns to pre-create level. Path resolves to NOT_FOUND. Create another file — verify it gets the same inode number (slot reused). Verify link_count accounting is correct throughout.
8. **Rename same-directory:** create `/a.txt`, write data, rename to `/b.txt`. `/a.txt` NOT_FOUND. `/b.txt` exists with original data.
9. **Rename cross-directory rejected:** `chaos_rename("/a.txt", "/subdir/a.txt")` returns `CHAOS_ERR_INVALID`.
10. **Rename over existing file:** create `/a.txt` and `/b.txt`. Rename `/a.txt` to `/b.txt`. Old `/b.txt` content is gone, new `/b.txt` has `/a.txt`'s content.
11. **Truncate expand:** create file with 100 bytes. `chaos_truncate(fd, 8192)`. `stat.size == 8192`. Read bytes 100-8191 — all zero.
12. **Truncate shrink:** create file with 100KB. `chaos_truncate(fd, 1024)`. `stat.size == 1024`. Free block count reflects freed blocks. Read beyond 1024 fails or returns 0 bytes.
13. **Seek basic:** write "ABCDEFGHIJ". Seek SET 5, read 3 → "FGH". Seek END 0, write "XYZ". Read full file from offset 0 → "ABCDEFGHIJXYZ".
14. **Seek past EOF + write:** create empty file. Seek to offset 8192. Write "X". `stat.size == 8193`. Read byte 0 → 0x00 (zero-filled gap). Read byte 8192 → 'X'.
15. **Disk full:** fill disk to 1 free block. Write enough to consume it. Next write returns `CHAOS_ERR_NO_SPACE`. Filesystem still mounts cleanly after hitting full.
16. **Inode exhaustion:** create files until `CHAOS_ERR_NO_INODES`. Unlink one. Create another — succeeds with reused inode number.
17. **Unlink-while-open:** open file for reading. Unlink the path. Verify path resolves to NOT_FOUND. Verify fd still reads original data. Close fd. Verify blocks are freed (free block count returns to baseline).
18. **fd table exhaustion:** open 16 files. 17th open returns `CHAOS_ERR_NO_FD`. Close one. 17th open succeeds.
19. **Dir handle exhaustion:** opendir 8 times. 9th returns `CHAOS_ERR_NO_FD`. closedir one. 9th succeeds.
20. **Dirty unmount recovery:** write data, kill QEMU. Remount. Verify `clean_unmount == 0` triggers fsck. fsck completes with 0 or correctable errors only. Data written before crash is intact.
21. **Backup superblock fallback:** corrupt block 0 (write garbage via ATA directly). Mount — falls back to block 1, succeeds. Block 0 is repaired (written back) after successful mount from backup.
22. **CRC-32 detection:** corrupt one byte of superblock at block 0. Mount — CRC mismatch → fallback to block 1, succeeds. Both blocks corrupt → `CHAOS_ERR_CORRUPT`.
23. **Link count correctness:** after creating and deleting a mix of files and directories, run fsck. Link count errors = 0.
24. **Inode cache hit rate:** open/stat/close the same file 100 times. Instrument `chaos_block_read()` call count — should not increase linearly (cache is serving hits after first load).
25. **fsck clean run:** after all above tests, `chaos_fsck()` returns 0.

---

## Summary

| Property | Value |
|----------|-------|
| Block size | 4096 bytes (fixed) |
| Max filename | 53 characters |
| Max file size | Disk-limited (extent limit is fragmentation ceiling, not size ceiling) |
| Max extents per file | 518 (6 inline + 512 indirect) |
| Max open files | 16 |
| Max open directories | 8 |
| Inode size | 128 bytes (32 per block) |
| Directory entry size | 64 bytes (64 per block) |
| Inode cache | 16 entries LRU, dirty write-back on eviction |
| Bitmap | Fully in-memory, write-through per word |
| Block cache | None (bitmap and inode cache are explicit; everything else is direct I/O) |
| Journaling | Not in v2 (journal fields reserved for v2.1) |
| Allocation strategy | Next-fit bitmap scan, 32-bit word granularity |
| Path type | Absolute only, max 512 chars, max 16 components |
| Rename scope | Same-directory only in v2 |
| Unlink-while-open | Supported (deferred free via open_count + unlink_pending) |
| Sparse files | Not supported — gaps are zero-filled and allocated |
| Symlinks | Reserved, not implemented in v2 |
| Time source | Seconds since boot (monotonic, not wall clock) |
| CRC algorithm | CRC-32/ISO-HDLC, polynomial 0xEDB88320 (zlib/Ethernet standard) |
| Null block sentinel | CHAOS_BLOCK_NULL = 0 |
| Null inode sentinel | CHAOS_INODE_NULL = 0 |
| Magic | 0x43484653 ('CHFS') |
| Version | 2 |

### Issues Fixed From First Draft

| # | Issue | Fix |
|---|-------|-----|
| 1 | Unlink-while-open undefined | open_count + unlink_pending deferred free |
| 2 | Directory handle table undefined | dir_handle_table[8] fully specified |
| 3 | chaos_rename cross-directory unspecified | Scoped to same-directory only, CHAOS_ERR_INVALID otherwise |
| 4 | Write ordering contradiction | Rule 1-4 write ordering contract, bitmap cache clearly explained |
| 5 | CHAOS_BLOCK_NULL not enforced | Named constant, all callers must check |
| 6 | Inode allocation condition wrong | magic != CHAOS_INODE_MAGIC only — never check link_count |
| 7 | Directory link_count rules missing | Full link count accounting rules specified |
| 8 | Dirty inode cache eviction | Mandatory write-back before eviction, explicitly stated |
| 9 | Seek-past-EOF undefined | Zero-fill-and-allocate, explicit non-sparse contract |
| 10 | Extent limit vs size limit tension | Clearly explained: fragmentation ceiling, not size ceiling |
| 11 | CRC32 polynomial unspecified | CRC-32/ISO-HDLC, 0xEDB88320 |
| 12 | Minimum format size is magic number | Derived minimum (5 blocks), practical floor (64 blocks) with rationale |
| 13 | chaos_closedir returns void | Returns int: CHAOS_OK or CHAOS_ERR_BAD_FD |
| 14 | Inode slot 0 waste undocumented | Explicitly documented as permanently reserved |
| 15 | chaos_sync() scope unclear | Flushes dirty inode cache entries only |
| 16 | Timestamp contradiction | All timestamps are seconds-since-boot, comments corrected throughout |
| 17 | chaos_format() caller undefined | Build system creates pre-formatted image; kernel does not auto-format |

**Do not proceed to Phase 5 until all Phase 4 acceptance tests pass.**
