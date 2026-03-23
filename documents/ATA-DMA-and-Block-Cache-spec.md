# ATA DMA & Block Cache — Specification
## AIOS v2 Disk Performance Subsystem

---

## Preface: Why This Matters

AIOS originally used ATA PIO (Programmed I/O) for all disk access. Every byte flowed through the CPU via `inw` on port 0x1F0 — one 16-bit word at a time. During a 4KB block read (8 sectors), the CPU executed 2,048 `in` instructions. The CPU was locked for the entire transfer.

The system now has a PCI bus driver, an E1000 NIC using DMA descriptor rings, and a graphical desktop running a compositor at 60fps. Disk I/O was the last major subsystem burning CPU on raw data movement.

**This spec covers two features:**

1. **ATA Bus Master DMA** — the disk controller transfers data directly to/from RAM. The CPU sets up the transfer, the DMA engine does the work, and an IRQ14 fires on completion.
2. **Block Cache** — a 512-entry LRU RAM cache in the ChaosFS block I/O layer. First read goes to disk via DMA. Every subsequent read of the same block hits RAM. Writes are write-through (always hit disk immediately).

**Rules:**
1. Every structure is a documented contract.
2. No silent failures — every operation returns a real error code.
3. DMA is the only data transfer path. PIO is used only for the IDENTIFY command at init (hardware protocol requirement — there is no DMA variant of IDENTIFY).
4. The block cache insertion point is `chaos_block_read()` / `chaos_block_write()` in `chaos_block.c`.

---

## Part 1: ATA Bus Master DMA

### Overview

ATA Bus Master DMA uses the IDE controller's Bus Master Interface (BMI) registers to perform disk-to-memory and memory-to-disk transfers without CPU involvement. The CPU's job is:

1. Allocate a physically contiguous bounce buffer and PRD table
2. Build a PRD (Physical Region Descriptor) table entry describing the transfer
3. Write the PRD table address to the BMI
4. Issue the DMA READ/WRITE command to the ATA drive
5. Start the DMA engine
6. Poll BMI status for completion (completes in microseconds)

The PCI IDE controller provides the Bus Master registers. AIOS has PCI bus enumeration in `drivers/pci.c`. The E1000 NIC module already demonstrates the pattern: PCI device discovery → BAR reading → bus mastering → DMA buffer allocation.

### Source Files

```
drivers/
├── ata.c / ata.h          # Init (PIO IDENTIFY), public read/write API (delegates to DMA)
├── ata_dma.c / ata_dma.h  # DMA init, PRD setup, DMA read/write, IRQ14 handler
```

### PCI IDE Controller Discovery

The IDE controller is PCI class 0x01 (Mass Storage), subclass 0x01 (IDE). On QEMU's i440FX/PIIX3 chipset, this is typically bus 0, device 1, function 1.

```c
#define PCI_CLASS_STORAGE     0x01
#define PCI_SUBCLASS_IDE      0x01

/* Bus Master Interface registers (I/O port offsets from BMI base) */
#define BMI_CMD_PRIMARY       0x00  /* Bus Master Command (R/W, 8-bit) */
#define BMI_STATUS_PRIMARY    0x02  /* Bus Master Status  (R/W, 8-bit) */
#define BMI_PRD_PRIMARY       0x04  /* PRD Table Address  (R/W, 32-bit) */

/* BMI Command register bits */
#define BMI_CMD_START         0x01  /* Start/stop DMA transfer */
#define BMI_CMD_WRITE         0x08  /* 0 = disk→RAM, 1 = RAM→disk */

/* BMI Status register bits */
#define BMI_STATUS_ACTIVE     0x01  /* DMA transfer in progress */
#define BMI_STATUS_ERROR      0x02  /* DMA error occurred */
#define BMI_STATUS_IRQ        0x04  /* Interrupt raised by drive */
```

### Init Sequence

```c
init_result_t ata_dma_init(void) {
    /* 1. Find PCI IDE controller via pci_find_class(0x01, 0x01) */
    /* 2. Read BAR4 — Bus Master Interface I/O base (bit 0 set = I/O BAR) */
    /* 3. Enable PCI Bus Mastering via pci_enable_bus_mastering() */
    /* 4. Allocate PRD table (1 page, 4KB, pmm_alloc_page + vmm_map_page) */
    /* 5. Allocate 64KB DMA bounce buffer (16 pages, pmm_alloc_pages + vmm_map_range) */
    /* 6. Register IRQ14 handler via irq_register_handler(14, handler) */
    /* 7. Clear BMI status register */
}
```

### PRD (Physical Region Descriptor) Table

```c
typedef struct {
    uint32_t phys_addr;   /* physical address of buffer (word-aligned) */
    uint16_t byte_count;  /* transfer size in bytes (0 = 64KB). Must be even. */
    uint16_t flags;       /* bit 15 = EOT (end of table) */
} __attribute__((packed)) prd_entry_t;

#define PRD_FLAG_EOT  0x8000
```

Single PRD entry per transfer. ChaosFS reads/writes are always 4KB-aligned, 4KB-sized, into the contiguous DMA bounce buffer.

### DMA Read Operation

```c
int ata_dma_read(uint32_t lba, uint32_t count, void* buffer) {
    /* 1. Set up PRD — single entry pointing to bounce buffer */
    /* 2. Stop any in-progress DMA, clear status */
    /* 3. Load PRD table address into BMI */
    /* 4. Set direction = read (disk → memory) */
    /* 5. Issue ATA READ DMA command (0xC8) to drive registers */
    /* 6. Start DMA engine (BMI_CMD_START) */
    /* 7. Poll BMI status until !(ACTIVE) && (IRQ) — completes in microseconds */
    /* 8. Stop DMA, clear status */
    /* 9. memcpy from bounce buffer to caller's buffer */
}
```

### DMA Write Operation

Same pattern as read, but:
- `memcpy` caller's data into bounce buffer first
- Set BMI direction = write (`BMI_CMD_WRITE`)
- Issue ATA WRITE DMA command (0xCA)

### IRQ14 Handler

The handler acknowledges the interrupt on both the BMI side (write 1 to clear IRQ bit) and the drive side (read ATA status register). This prevents IRQ14 from staying asserted.

### Completion: Polled

DMA completion is always polled. The transfer completes in microseconds (both in QEMU and on real hardware for modern IDE controllers). The polling loop checks BMI status for `!(ACTIVE) && (IRQ)` — typically completes within a few iterations.

### Integration with ATA Driver

`ata_read_sectors()` and `ata_write_sectors()` in `ata.c` delegate directly to `ata_dma_read()` / `ata_dma_write()`. PIO read/write code has been removed. The only remaining PIO usage is the IDENTIFY command in `ata_init()`, which is a hardware protocol requirement (runs before DMA exists).

```c
int ata_read_sectors(uint32_t lba, uint32_t count, void* buffer) {
    /* validation */
    return ata_dma_read(lba, count, buffer);
}
```

### Concurrency

Single ATA bus, single DMA transfer at a time. The polling completion means the caller blocks until the transfer finishes (microseconds). No locking needed.

### Boot Sequence

```c
/* In kernel_main(), Phase 3 */
boot_log("ATA/IDE PIO disk", ata_init());   /* IDENTIFY via PIO */
boot_log("PCI bus",          pci_init());    /* Enumerate PCI devices */
boot_log("ATA DMA",          ata_dma_init()); /* Set up DMA engine */
```

DMA init runs after both `ata_init()` (drive detection) and `pci_init()` (PCI enumeration). If DMA init fails (no PCI IDE controller found), `ata_dma_available()` returns false and `ata_read/write_sectors()` will fail — there is no PIO fallback for data transfers.

---

## Part 2: Block Cache

### Overview

The block cache sits inside the ChaosFS block I/O layer (`chaos_block.c`). It is transparent to all filesystem code above it.

- **512 entries** × 4KB = **2MB RAM**
- **LRU eviction** with monotonic counter
- **Write-through** — every write hits disk immediately, cache is always clean
- **O(1) lookup** via open-addressing hash table (1024 slots, 50% load factor)
- **Automatic invalidation** — `chaos_free_block()` invalidates the cache entry

### Source Files

```
kernel/chaos/
├── chaos_block.c     # Block I/O layer — raw + cached wrappers
├── block_cache.c     # Cache implementation (init, lookup, evict, stats)
├── block_cache.h     # Public API
```

### Cache Entry

```c
#define BLOCK_CACHE_SIZE     512
#define BLOCK_CACHE_INVALID  0xFFFFFFFF

typedef struct {
    uint32_t block_idx;     /* ChaosFS block index, or INVALID if empty */
    uint8_t* data;          /* 4KB buffer (PMM-allocated, identity-mapped) */
    uint32_t lru_counter;   /* higher = more recently used */
    bool     valid;
} cache_entry_t;
```

### Hash Table

Open-addressing hash table for O(1) lookup. 1024 slots for 512 max entries = 50% load factor. Multiplicative hash function. Backward-shift deletion to maintain probe chains.

```c
#define CACHE_HASH_SIZE   1024
static uint16_t cache_hash[CACHE_HASH_SIZE];  /* value = index into cache[] */

static inline uint32_t cache_hash_fn(uint32_t block_idx) {
    return (block_idx * 2654435761u) >> 22;  /* Knuth multiplicative hash */
}
```

### Block I/O Layering

`chaos_block.c` provides two layers:

```c
/* Raw — direct ATA access, used by cache on miss/write-through */
int chaos_block_read_raw(uint32_t block_idx, void* buffer);
int chaos_block_write_raw(uint32_t block_idx, const void* buffer);

/* Public — goes through cache if enabled */
int chaos_block_read(uint32_t block_idx, void* buffer);
int chaos_block_write(uint32_t block_idx, const void* buffer);
```

### Cached Read

```c
int block_cache_read(uint32_t block_idx, void* buffer) {
    /* 1. Hash lookup → if hit: memcpy from cache, touch LRU, stats.hits++ */
    /* 2. Miss: read from disk via chaos_block_read_raw() */
    /* 3. Find LRU slot (or empty slot), evict if needed */
    /* 4. Copy data into cache slot, insert into hash table */
}
```

### Cached Write (Write-Through)

```c
int block_cache_write(uint32_t block_idx, const void* buffer) {
    /* 1. Write to disk FIRST via chaos_block_write_raw() */
    /* 2. If block is cached: update cache copy, touch LRU */
    /* 3. If not cached: write-allocate (populate cache with this data) */
}
```

### Invalidation

Called automatically from `chaos_free_block()` in `chaos_alloc.c`. This single insertion point catches all block frees across unlink, truncate, and directory operations.

```c
void block_cache_invalidate(uint32_t block_idx);  /* remove one entry */
void block_cache_flush(void);                       /* clear entire cache */
```

### Stats

```c
typedef struct {
    uint32_t hits;
    uint32_t misses;
    uint32_t evictions;
    uint32_t write_throughs;
} cache_stats_t;

cache_stats_t block_cache_get_stats(void);
int           block_cache_hit_rate(void);  /* 0-100 */
```

### Lua API

```lua
aios.os.cache_stats()  -- returns {hits, misses, evictions, write_throughs, hit_rate}
aios.os.cache_flush()  -- clears entire block cache
```

### Boot Sequence

```c
/* In kernel_main(), before chaos_mount() */
boot_log("Block cache", block_cache_init());
```

If `block_cache_init()` fails (OOM), `block_cache_is_enabled()` returns false and `chaos_block_read/write()` go directly to raw disk access.

### Memory Cost

512 entries × 4KB = 2MB data buffers + 2KB hash table + ~12KB cache_entry_t array = ~2.01MB total. On a 256MB system, negligible.

---

## Combined Boot Sequence

```c
/* Phase 3: Drivers */
boot_log("ATA/IDE PIO disk",  ata_init());       /* IDENTIFY command */
boot_log("PCI bus",            pci_init());        /* Enumerate PCI */
boot_log("ATA DMA",            ata_dma_init());    /* DMA engine setup */

/* Phase 4: ChaosFS */
boot_log("Block cache",        block_cache_init()); /* 2MB LRU cache */
boot_log("ChaosFS",            chaos_mount(2048));   /* Mount filesystem */
```

---

## Performance Impact

| Scenario | Before (PIO, no cache) | After (DMA + cache) |
|----------|----------------------|---------------------|
| First read of a 4KB block | CPU stalled ~2-4ms (PIO) | DMA transfer, CPU polls for µs |
| Second read of same block | CPU stalled ~2-4ms (PIO again) | ~1µs memcpy from cache |
| Loading a Lua script (10 blocks) | ~20-40ms CPU stall | First load: DMA. All subsequent: ~10µs from cache |
| Steady-state desktop (apps loaded) | Repeated disk reads | >95% cache hit rate — disk nearly silent |
