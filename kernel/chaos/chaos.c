/* AIOS v2 — ChaosFS Core
 * Mount/unmount, file descriptor table, open/close/read/write/seek/truncate/unlink.
 * This is the largest ChaosFS source file. */

#include "chaos.h"
#include "chaos_types.h"
#include "../../include/string.h"
#include "../../drivers/serial.h"
#include "../../drivers/timer.h"
#include "../../kernel/heap.h"

/* Extern from chaos modules */
extern void     chaos_block_set_lba(uint32_t lba_start);
extern int      chaos_block_read(uint32_t block_idx, void* buffer);
extern int      chaos_block_write(uint32_t block_idx, const void* buffer);
extern int      chaos_alloc_init(const struct chaos_superblock* sb);
extern void     chaos_alloc_shutdown(void);
extern uint32_t chaos_alloc_block(void);
extern void     chaos_free_block(uint32_t block_idx);
extern uint32_t chaos_alloc_inode(void);
extern void     chaos_free_inode(uint32_t inode_num);
extern uint32_t chaos_get_free_blocks(void);
extern uint32_t chaos_get_free_inodes(void);
extern uint32_t chaos_get_total_blocks(void);
extern uint32_t chaos_get_data_start(void);
extern void     chaos_inode_init(void);
extern int      chaos_inode_read(uint32_t inode_num, struct chaos_inode* out);
extern int      chaos_inode_write(uint32_t inode_num, const struct chaos_inode* ino);
extern int      chaos_inode_write_through(uint32_t inode_num, const struct chaos_inode* ino);
extern int      chaos_inode_flush(void);
extern void     chaos_inode_invalidate(void);
extern uint32_t chaos_resolve_path(const char* path);
extern int      chaos_dir_find(uint32_t dir_inode_num, const char* name,
                               struct chaos_dirent* out_dirent,
                               uint32_t* out_block_idx, uint32_t* out_slot);
extern int      chaos_dir_add(uint32_t dir_inode_num, const char* name,
                              uint32_t target_inode, uint8_t type);
extern int      chaos_dir_remove(uint32_t dir_inode_num, const char* name);
extern void     chaos_dir_init(void);
extern uint32_t chaos_crc32(const void* data, size_t len);
extern int      chaos_format(uint32_t lba_start, uint32_t lba_count, const char* label);

/* ── State ─────────────────────────────────────────── */

static struct chaos_superblock sb;
static bool mounted = false;
static uint32_t mount_lba_start;
static struct chaos_fd fd_table[CHAOS_MAX_FD];
static char fs_label[17];

/* ── Superblock helpers ────────────────────────────── */

static int write_superblock(void) {
    sb.checksum = chaos_crc32(&sb, (size_t)((uint8_t*)&sb.checksum - (uint8_t*)&sb));

    uint8_t* buf = (uint8_t*)kzmalloc(CHAOS_BLOCK_SIZE);
    if (!buf) return CHAOS_ERR_IO;
    memcpy(buf, &sb, sizeof(sb));

    int r1 = chaos_block_write(0, buf);
    int r2 = chaos_block_write(1, buf);
    kfree(buf);
    return (r1 == CHAOS_OK && r2 == CHAOS_OK) ? CHAOS_OK : CHAOS_ERR_IO;
}

/* ── Extent walk helper ────────────────────────────── */

static uint32_t inode_logical_block(const struct chaos_inode* ino, uint32_t logical) {
    uint32_t pos = 0;
    for (int i = 0; i < ino->extent_count && i < CHAOS_MAX_INLINE_EXTENTS; i++) {
        if (logical < pos + ino->extents[i].block_count) {
            return ino->extents[i].start_block + (logical - pos);
        }
        pos += ino->extents[i].block_count;
    }
    return CHAOS_BLOCK_NULL;
}

static uint32_t inode_block_count(const struct chaos_inode* ino) {
    uint32_t total = 0;
    for (int i = 0; i < ino->extent_count && i < CHAOS_MAX_INLINE_EXTENTS; i++) {
        total += ino->extents[i].block_count;
    }
    return total;
}

/* Try to add a block to the inode's extent list. Returns 0 on success. */
static int inode_add_block(struct chaos_inode* ino, uint32_t block_idx) {
    /* Try to extend last extent if contiguous */
    if (ino->extent_count > 0) {
        struct chaos_extent* last = &ino->extents[ino->extent_count - 1];
        if (last->start_block + last->block_count == block_idx) {
            last->block_count++;
            return 0;
        }
    }
    /* Add new extent */
    if (ino->extent_count < CHAOS_MAX_INLINE_EXTENTS) {
        ino->extents[ino->extent_count].start_block = block_idx;
        ino->extents[ino->extent_count].block_count = 1;
        ino->extent_count++;
        return 0;
    }
    return -1;  /* extent list full */
}

/* Free all blocks owned by an inode */
static void free_inode_blocks(struct chaos_inode* ino) {
    for (int i = 0; i < ino->extent_count && i < CHAOS_MAX_INLINE_EXTENTS; i++) {
        for (uint32_t b = 0; b < ino->extents[i].block_count; b++) {
            chaos_free_block(ino->extents[i].start_block + b);
        }
    }
}

/* ── Mount / Unmount ───────────────────────────────── */

int chaos_mount(uint32_t lba_start) {
    if (mounted) return CHAOS_ERR_INVALID;

    chaos_block_set_lba(lba_start);
    mount_lba_start = lba_start;

    uint8_t* buf = (uint8_t*)kmalloc(CHAOS_BLOCK_SIZE);
    if (!buf) return CHAOS_ERR_IO;

    /* Try primary superblock (block 0) */
    bool primary_ok = false;
    if (chaos_block_read(0, buf) == CHAOS_OK) {
        memcpy(&sb, buf, sizeof(sb));
        if (sb.magic == CHAOS_MAGIC && sb.version == CHAOS_VERSION &&
            sb.block_size == CHAOS_BLOCK_SIZE) {
            uint32_t crc = chaos_crc32(&sb, (size_t)((uint8_t*)&sb.checksum - (uint8_t*)&sb));
            if (crc == sb.checksum) primary_ok = true;
        }
    }

    if (!primary_ok) {
        serial_print("[chaosfs] primary superblock corrupt, trying backup\n");
        if (chaos_block_read(1, buf) == CHAOS_OK) {
            memcpy(&sb, buf, sizeof(sb));
            if (sb.magic == CHAOS_MAGIC && sb.version == CHAOS_VERSION &&
                sb.block_size == CHAOS_BLOCK_SIZE) {
                uint32_t crc = chaos_crc32(&sb, (size_t)((uint8_t*)&sb.checksum - (uint8_t*)&sb));
                if (crc != sb.checksum) {
                    kfree(buf);
                    return CHAOS_ERR_CORRUPT;
                }
            } else {
                kfree(buf);
                return CHAOS_ERR_CORRUPT;
            }
        } else {
            kfree(buf);
            return CHAOS_ERR_CORRUPT;
        }
    }
    kfree(buf);

    /* Initialize subsystems */
    chaos_inode_init();
    chaos_dir_init();
    memset(fd_table, 0, sizeof(fd_table));

    int r = chaos_alloc_init(&sb);
    if (r != CHAOS_OK) return r;

    /* Check clean unmount flag */
    if (!sb.clean_unmount) {
        serial_print("[chaosfs] dirty unmount detected, running fsck\n");
        chaos_fsck();
    }

    /* Update mount state */
    sb.clean_unmount = 0;
    sb.mounted = 1;
    sb.mount_count++;
    sb.last_mounted_time = (uint32_t)(timer_get_ticks() / timer_get_frequency());
    write_superblock();

    strncpy(fs_label, sb.fs_name, 16);
    fs_label[16] = '\0';

    mounted = true;
    serial_printf("[chaosfs] mounted: %u blocks, %u free, %u inodes\n",
                  sb.total_blocks, sb.free_blocks, sb.total_inodes);
    return CHAOS_OK;
}

void chaos_unmount(void) {
    if (!mounted) return;

    /* Close any open file descriptors */
    for (int i = 0; i < CHAOS_MAX_FD; i++) {
        if (fd_table[i].in_use) {
            chaos_close(i);
        }
    }

    chaos_inode_flush();

    sb.clean_unmount = 1;
    sb.mounted = 0;
    sb.free_blocks = chaos_get_free_blocks();
    sb.free_inodes = chaos_get_free_inodes();
    write_superblock();

    chaos_alloc_shutdown();
    mounted = false;
    serial_print("[chaosfs] unmounted cleanly\n");
}

void chaos_sync(void) {
    if (!mounted) return;
    chaos_inode_flush();
    sb.free_blocks = chaos_get_free_blocks();
    sb.free_inodes = chaos_get_free_inodes();
    write_superblock();
}

/* ── File operations ───────────────────────────────── */

static uint32_t get_timestamp(void) {
    return (uint32_t)(timer_get_ticks() / timer_get_frequency());
}

/* Resolve parent dir and name from path */
static uint32_t resolve_parent_and_name(const char* path, char* out_name) {
    if (!path || path[0] != '/' || strlen(path) >= CHAOS_MAX_PATH) return CHAOS_INODE_NULL;
    const char* last_slash = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }
    const char* name = last_slash + 1;
    if (*name == '\0') return CHAOS_INODE_NULL;
    strncpy(out_name, name, CHAOS_MAX_FILENAME);
    out_name[CHAOS_MAX_FILENAME] = '\0';

    if (last_slash == path) return 1;  /* parent is root */

    char parent_path[CHAOS_MAX_PATH];
    size_t plen = (size_t)(last_slash - path);
    memcpy(parent_path, path, plen);
    parent_path[plen] = '\0';
    return chaos_resolve_path(parent_path);
}

int chaos_open(const char* path, int flags) {
    if (!mounted) return CHAOS_ERR_NOT_MOUNTED;
    if (!path || path[0] != '/') return CHAOS_ERR_INVALID;

    uint8_t fd_flags = 0;
    if (flags & CHAOS_O_RDONLY) fd_flags |= FD_FLAG_READ;
    if (flags & CHAOS_O_WRONLY) fd_flags |= FD_FLAG_WRITE;
    if (flags & CHAOS_O_RDWR)  fd_flags |= (FD_FLAG_READ | FD_FLAG_WRITE);
    if (flags & CHAOS_O_APPEND) fd_flags |= FD_FLAG_APPEND;

    uint32_t ino_num = chaos_resolve_path(path);

    if (ino_num == CHAOS_INODE_NULL) {
        if (!(flags & CHAOS_O_CREAT)) return CHAOS_ERR_NOT_FOUND;

        /* Create new file */
        char name[CHAOS_MAX_FILENAME + 1];
        uint32_t parent = resolve_parent_and_name(path, name);
        if (parent == CHAOS_INODE_NULL) return CHAOS_ERR_INVALID;

        ino_num = chaos_alloc_inode();
        if (ino_num == CHAOS_INODE_NULL) return CHAOS_ERR_NO_INODES;

        struct chaos_inode new_ino;
        memset(&new_ino, 0, sizeof(new_ino));
        new_ino.magic = CHAOS_INODE_MAGIC;
        new_ino.mode = CHAOS_TYPE_FILE | 0644;
        new_ino.link_count = 1;
        new_ino.created_time = get_timestamp();
        new_ino.modified_time = new_ino.created_time;

        /* Write inode before directory entry (write ordering rule 2) */
        chaos_inode_write_through(ino_num, &new_ino);

        int r = chaos_dir_add(parent, name, ino_num, CHAOS_DT_FILE);
        if (r != CHAOS_OK) {
            chaos_free_inode(ino_num);
            return r;
        }
    }

    /* Verify it's a file */
    struct chaos_inode ino;
    if (chaos_inode_read(ino_num, &ino) != CHAOS_OK) return CHAOS_ERR_IO;
    if ((ino.mode & CHAOS_TYPE_MASK) == CHAOS_TYPE_DIR) return CHAOS_ERR_NOT_FILE;

    /* Find free fd */
    int fd = -1;
    for (int i = 0; i < CHAOS_MAX_FD; i++) {
        if (!fd_table[i].in_use) { fd = i; break; }
    }
    if (fd < 0) return CHAOS_ERR_NO_FD;

    fd_table[fd].in_use = true;
    fd_table[fd].inode_num = ino_num;
    fd_table[fd].position = 0;
    fd_table[fd].flags = fd_flags;

    /* Increment open_count */
    ino.open_count++;
    chaos_inode_write(ino_num, &ino);

    /* Handle O_TRUNC */
    if ((flags & CHAOS_O_TRUNC) && (fd_flags & FD_FLAG_WRITE)) {
        chaos_truncate(fd, 0);
    }

    /* Handle O_APPEND */
    if (fd_flags & FD_FLAG_APPEND) {
        fd_table[fd].position = ino.size;
    }

    return fd;
}

int chaos_close(int fd) {
    if (fd < 0 || fd >= CHAOS_MAX_FD || !fd_table[fd].in_use) return CHAOS_ERR_BAD_FD;

    uint32_t ino_num = fd_table[fd].inode_num;
    fd_table[fd].in_use = false;

    struct chaos_inode ino;
    if (chaos_inode_read(ino_num, &ino) == CHAOS_OK) {
        if (ino.open_count > 0) ino.open_count--;

        if (ino.open_count == 0 && ino.unlink_pending) {
            /* Deferred unlink — free everything now */
            free_inode_blocks(&ino);
            chaos_free_inode(ino_num);
        } else {
            chaos_inode_write(ino_num, &ino);
        }
    }

    return CHAOS_OK;
}

int chaos_read(int fd, void* buf, uint32_t len) {
    if (fd < 0 || fd >= CHAOS_MAX_FD || !fd_table[fd].in_use) return CHAOS_ERR_BAD_FD;
    if (!(fd_table[fd].flags & FD_FLAG_READ)) return CHAOS_ERR_READ_ONLY;
    if (!buf || len == 0) return 0;

    struct chaos_inode ino;
    if (chaos_inode_read(fd_table[fd].inode_num, &ino) != CHAOS_OK) return CHAOS_ERR_IO;

    uint64_t pos = fd_table[fd].position;
    if (pos >= ino.size) return 0;
    if (pos + len > ino.size) len = (uint32_t)(ino.size - pos);

    uint8_t* block_buf = (uint8_t*)kmalloc(CHAOS_BLOCK_SIZE);
    if (!block_buf) return CHAOS_ERR_IO;

    uint32_t bytes_read = 0;
    while (bytes_read < len) {
        uint32_t logical_block = (uint32_t)((pos + bytes_read) / CHAOS_BLOCK_SIZE);
        uint32_t offset_in_block = (uint32_t)((pos + bytes_read) % CHAOS_BLOCK_SIZE);
        uint32_t phys = inode_logical_block(&ino, logical_block);
        if (phys == CHAOS_BLOCK_NULL) break;

        if (chaos_block_read(phys, block_buf) != CHAOS_OK) break;

        uint32_t to_copy = CHAOS_BLOCK_SIZE - offset_in_block;
        if (to_copy > len - bytes_read) to_copy = len - bytes_read;

        memcpy((uint8_t*)buf + bytes_read, block_buf + offset_in_block, to_copy);
        bytes_read += to_copy;
    }

    kfree(block_buf);
    fd_table[fd].position += bytes_read;
    return (int)bytes_read;
}

int chaos_write(int fd, const void* buf, uint32_t len) {
    if (fd < 0 || fd >= CHAOS_MAX_FD || !fd_table[fd].in_use) return CHAOS_ERR_BAD_FD;
    if (!(fd_table[fd].flags & FD_FLAG_WRITE)) return CHAOS_ERR_READ_ONLY;
    if (!buf || len == 0) return 0;

    struct chaos_inode ino;
    uint32_t ino_num = fd_table[fd].inode_num;
    if (chaos_inode_read(ino_num, &ino) != CHAOS_OK) return CHAOS_ERR_IO;

    uint64_t pos = fd_table[fd].position;
    if (fd_table[fd].flags & FD_FLAG_APPEND) pos = ino.size;

    uint8_t* block_buf = (uint8_t*)kmalloc(CHAOS_BLOCK_SIZE);
    if (!block_buf) return CHAOS_ERR_IO;

    /* If writing past EOF, we need to allocate blocks for the gap */
    uint32_t bytes_written = 0;
    while (bytes_written < len) {
        uint64_t write_pos = pos + bytes_written;
        uint32_t logical_block = (uint32_t)(write_pos / CHAOS_BLOCK_SIZE);
        uint32_t offset_in_block = (uint32_t)(write_pos % CHAOS_BLOCK_SIZE);

        uint32_t phys = inode_logical_block(&ino, logical_block);

        if (phys == CHAOS_BLOCK_NULL) {
            /* Need to allocate blocks up to and including this logical block */
            uint32_t current_blocks = inode_block_count(&ino);
            while (current_blocks <= logical_block) {
                uint32_t new_block = chaos_alloc_block();
                if (new_block == CHAOS_BLOCK_NULL) {
                    /* Commit what we have so far */
                    goto write_done;
                }
                /* Zero the new block (for gap filling) */
                memset(block_buf, 0, CHAOS_BLOCK_SIZE);
                chaos_block_write(new_block, block_buf);

                if (inode_add_block(&ino, new_block) != 0) {
                    chaos_free_block(new_block);
                    goto write_done;
                }
                current_blocks++;
            }
            phys = inode_logical_block(&ino, logical_block);
            if (phys == CHAOS_BLOCK_NULL) goto write_done;
        }

        /* Read existing block (may need partial write) */
        if (offset_in_block != 0 || (len - bytes_written) < CHAOS_BLOCK_SIZE) {
            if (chaos_block_read(phys, block_buf) != CHAOS_OK) {
                memset(block_buf, 0, CHAOS_BLOCK_SIZE);
            }
        }

        uint32_t to_copy = CHAOS_BLOCK_SIZE - offset_in_block;
        if (to_copy > len - bytes_written) to_copy = len - bytes_written;

        memcpy(block_buf + offset_in_block, (const uint8_t*)buf + bytes_written, to_copy);

        /* Write data block FIRST (write ordering rule 1) */
        if (chaos_block_write(phys, block_buf) != CHAOS_OK) break;

        bytes_written += to_copy;
    }

write_done:
    kfree(block_buf);

    /* Update inode metadata AFTER data (write ordering rule 1) */
    uint64_t new_end = pos + bytes_written;
    if (new_end > ino.size) ino.size = new_end;
    ino.modified_time = get_timestamp();
    chaos_inode_write_through(ino_num, &ino);

    fd_table[fd].position = new_end;
    return (int)bytes_written;
}

int64_t chaos_seek(int fd, int64_t offset, int whence) {
    if (fd < 0 || fd >= CHAOS_MAX_FD || !fd_table[fd].in_use) return CHAOS_ERR_BAD_FD;

    struct chaos_inode ino;
    if (chaos_inode_read(fd_table[fd].inode_num, &ino) != CHAOS_OK) return CHAOS_ERR_IO;

    int64_t new_pos;
    switch (whence) {
        case CHAOS_SEEK_SET: new_pos = offset; break;
        case CHAOS_SEEK_CUR: new_pos = (int64_t)fd_table[fd].position + offset; break;
        case CHAOS_SEEK_END: new_pos = (int64_t)ino.size + offset; break;
        default: return CHAOS_ERR_INVALID;
    }

    if (new_pos < 0) return CHAOS_ERR_INVALID;
    fd_table[fd].position = (uint64_t)new_pos;
    return new_pos;
}

int chaos_truncate(int fd, uint64_t size) {
    if (fd < 0 || fd >= CHAOS_MAX_FD || !fd_table[fd].in_use) return CHAOS_ERR_BAD_FD;
    if (!(fd_table[fd].flags & FD_FLAG_WRITE)) return CHAOS_ERR_READ_ONLY;

    uint32_t ino_num = fd_table[fd].inode_num;
    struct chaos_inode ino;
    if (chaos_inode_read(ino_num, &ino) != CHAOS_OK) return CHAOS_ERR_IO;

    if (size > ino.size) {
        /* Expand: allocate blocks to cover new size, zero-fill */
        uint32_t needed_blocks = (uint32_t)((size + CHAOS_BLOCK_SIZE - 1) / CHAOS_BLOCK_SIZE);
        uint32_t current_blocks = inode_block_count(&ino);

        uint8_t* zero_buf = (uint8_t*)kzmalloc(CHAOS_BLOCK_SIZE);
        if (!zero_buf) return CHAOS_ERR_IO;

        while (current_blocks < needed_blocks) {
            uint32_t new_block = chaos_alloc_block();
            if (new_block == CHAOS_BLOCK_NULL) { kfree(zero_buf); return CHAOS_ERR_NO_SPACE; }
            chaos_block_write(new_block, zero_buf);
            if (inode_add_block(&ino, new_block) != 0) {
                chaos_free_block(new_block);
                kfree(zero_buf);
                return CHAOS_ERR_NO_SPACE;
            }
            current_blocks++;
        }
        kfree(zero_buf);
    } else if (size < ino.size) {
        /* Shrink: free blocks beyond new size */
        uint32_t keep_blocks = (uint32_t)((size + CHAOS_BLOCK_SIZE - 1) / CHAOS_BLOCK_SIZE);
        uint32_t pos = 0;

        for (int i = 0; i < ino.extent_count && i < CHAOS_MAX_INLINE_EXTENTS; i++) {
            uint32_t ext_start = pos;
            uint32_t ext_end = pos + ino.extents[i].block_count;

            if (ext_start >= keep_blocks) {
                /* Free entire extent */
                for (uint32_t b = 0; b < ino.extents[i].block_count; b++) {
                    chaos_free_block(ino.extents[i].start_block + b);
                }
                ino.extents[i].start_block = 0;
                ino.extents[i].block_count = 0;
            } else if (ext_end > keep_blocks) {
                /* Partially free this extent */
                uint32_t keep = keep_blocks - ext_start;
                for (uint32_t b = keep; b < ino.extents[i].block_count; b++) {
                    chaos_free_block(ino.extents[i].start_block + b);
                }
                ino.extents[i].block_count = keep;
            }
            pos = ext_end;
        }

        /* Compact extent list (remove empty extents) */
        int write_idx = 0;
        for (int i = 0; i < ino.extent_count && i < CHAOS_MAX_INLINE_EXTENTS; i++) {
            if (ino.extents[i].block_count > 0) {
                if (write_idx != i) ino.extents[write_idx] = ino.extents[i];
                write_idx++;
            }
        }
        ino.extent_count = (uint8_t)write_idx;
        /* Zero remaining extent slots */
        for (int i = write_idx; i < CHAOS_MAX_INLINE_EXTENTS; i++) {
            ino.extents[i].start_block = 0;
            ino.extents[i].block_count = 0;
        }
    }

    ino.size = size;
    ino.modified_time = get_timestamp();
    chaos_inode_write_through(ino_num, &ino);
    return CHAOS_OK;
}

int chaos_unlink(const char* path) {
    if (!mounted) return CHAOS_ERR_NOT_MOUNTED;

    char name[CHAOS_MAX_FILENAME + 1];
    uint32_t parent = resolve_parent_and_name(path, name);
    if (parent == CHAOS_INODE_NULL) return CHAOS_ERR_INVALID;

    struct chaos_dirent entry;
    if (chaos_dir_find(parent, name, &entry, NULL, NULL) < 0) {
        return CHAOS_ERR_NOT_FOUND;
    }
    if (entry.type == CHAOS_DT_DIR) return CHAOS_ERR_NOT_FILE;

    /* Remove directory entry */
    chaos_dir_remove(parent, name);

    /* Update inode */
    struct chaos_inode ino;
    if (chaos_inode_read(entry.inode, &ino) != CHAOS_OK) return CHAOS_ERR_IO;

    if (ino.link_count > 0) ino.link_count--;

    if (ino.link_count == 0 && ino.open_count == 0) {
        free_inode_blocks(&ino);
        chaos_free_inode(entry.inode);
    } else if (ino.link_count == 0) {
        ino.unlink_pending = true;
        chaos_inode_write(entry.inode, &ino);
    } else {
        chaos_inode_write(entry.inode, &ino);
    }

    return CHAOS_OK;
}

/* ── Stat ──────────────────────────────────────────── */

static int fill_stat(uint32_t ino_num, struct chaos_stat* st) {
    struct chaos_inode ino;
    if (chaos_inode_read(ino_num, &ino) != CHAOS_OK) return CHAOS_ERR_IO;

    st->inode = ino_num;
    st->mode = ino.mode;
    st->size = ino.size;
    st->block_count = inode_block_count(&ino);
    st->created_time = ino.created_time;
    st->modified_time = ino.modified_time;
    st->accessed_time = ino.accessed_time;
    return CHAOS_OK;
}

int chaos_stat(const char* path, struct chaos_stat* st) {
    if (!mounted) return CHAOS_ERR_NOT_MOUNTED;
    if (!path || !st) return CHAOS_ERR_INVALID;
    uint32_t ino = chaos_resolve_path(path);
    if (ino == CHAOS_INODE_NULL) return CHAOS_ERR_NOT_FOUND;
    return fill_stat(ino, st);
}

int chaos_fstat(int fd, struct chaos_stat* st) {
    if (fd < 0 || fd >= CHAOS_MAX_FD || !fd_table[fd].in_use) return CHAOS_ERR_BAD_FD;
    if (!st) return CHAOS_ERR_INVALID;
    return fill_stat(fd_table[fd].inode_num, st);
}

/* ── Utility ───────────────────────────────────────── */

uint32_t    chaos_free_blocks(void) { return mounted ? chaos_get_free_blocks() : 0; }
uint32_t    chaos_free_inodes(void) { return mounted ? chaos_get_free_inodes() : 0; }
uint32_t    chaos_total_blocks(void) { return mounted ? chaos_get_total_blocks() : 0; }
const char* chaos_label(void) { return mounted ? fs_label : ""; }

/* ── Expose mounted state for fsck ─────────────────── */

bool chaos_is_mounted(void) { return mounted; }
struct chaos_superblock* chaos_get_superblock(void) { return &sb; }
