/* AIOS v2 — ChaosFS Directory Operations
 * Path resolution, mkdir/rmdir, opendir/readdir/closedir, rename. */

#include "chaos_types.h"
#include "../../include/string.h"
#include "../../drivers/serial.h"
#include "../../kernel/heap.h"

/* Extern from other chaos modules */
extern int      chaos_block_read(uint32_t block_idx, void* buffer);
extern int      chaos_block_write(uint32_t block_idx, const void* buffer);
extern uint32_t chaos_alloc_block(void);
extern void     chaos_free_block(uint32_t block_idx);
extern uint32_t chaos_alloc_inode(void);
extern void     chaos_free_inode(uint32_t inode_num);
extern int      chaos_inode_read(uint32_t inode_num, struct chaos_inode* out);
extern int      chaos_inode_write(uint32_t inode_num, const struct chaos_inode* ino);
extern int      chaos_inode_write_through(uint32_t inode_num, const struct chaos_inode* ino);

/* Forward declaration */
uint32_t chaos_resolve_path(const char* path);

/* ── Dir handle table ──────────────────────────────── */

static struct chaos_dir_handle dir_handles[CHAOS_MAX_DIR_HANDLES];

/* ── Extent walker helper ──────────────────────────── */

/* Get the nth logical block from an inode's extent list.
 * Returns the physical block index, or CHAOS_BLOCK_NULL if not found. */
static uint32_t inode_logical_block(const struct chaos_inode* ino, uint32_t logical) {
    uint32_t pos = 0;
    for (int i = 0; i < ino->extent_count && i < CHAOS_MAX_INLINE_EXTENTS; i++) {
        if (logical < pos + ino->extents[i].block_count) {
            return ino->extents[i].start_block + (logical - pos);
        }
        pos += ino->extents[i].block_count;
    }
    /* TODO: indirect extent block support */
    return CHAOS_BLOCK_NULL;
}

/* Get total blocks in an inode's extent list */
static uint32_t inode_total_blocks(const struct chaos_inode* ino) {
    uint32_t total = 0;
    for (int i = 0; i < ino->extent_count && i < CHAOS_MAX_INLINE_EXTENTS; i++) {
        total += ino->extents[i].block_count;
    }
    return total;
}

/* ── Directory entry search ────────────────────────── */

/* Find a directory entry by name within a directory inode.
 * Returns: slot index (0-based across all blocks) on success, -1 if not found.
 * If out_dirent is non-NULL, copies the found entry into it.
 * If out_block and out_slot are non-NULL, returns the block and slot indices. */
int chaos_dir_find(uint32_t dir_inode_num, const char* name,
                   struct chaos_dirent* out_dirent,
                   uint32_t* out_block_idx, uint32_t* out_slot) {
    struct chaos_inode dir_ino;
    if (chaos_inode_read(dir_inode_num, &dir_ino) != CHAOS_OK) return -1;
    if (!(dir_ino.mode & CHAOS_TYPE_DIR)) return -1;

    uint32_t total = inode_total_blocks(&dir_ino);
    uint8_t* buf = (uint8_t*)kmalloc(CHAOS_BLOCK_SIZE);
    if (!buf) return -1;

    size_t name_len = strlen(name);

    for (uint32_t b = 0; b < total; b++) {
        uint32_t phys = inode_logical_block(&dir_ino, b);
        if (phys == CHAOS_BLOCK_NULL) continue;
        if (chaos_block_read(phys, buf) != CHAOS_OK) continue;

        for (uint32_t s = 0; s < CHAOS_DIRENTS_PER_BLK; s++) {
            struct chaos_dirent* d = (struct chaos_dirent*)(buf + s * CHAOS_DIRENT_SIZE);
            if (d->inode == CHAOS_INODE_NULL) continue;
            if (d->name_len == name_len && strncmp(d->name, name, name_len) == 0) {
                if (out_dirent) *out_dirent = *d;
                if (out_block_idx) *out_block_idx = b;
                if (out_slot) *out_slot = s;
                kfree(buf);
                return (int)(b * CHAOS_DIRENTS_PER_BLK + s);
            }
        }
    }

    kfree(buf);
    return -1;
}

/* Add a directory entry to a directory */
int chaos_dir_add(uint32_t dir_inode_num, const char* name,
                  uint32_t target_inode, uint8_t type) {
    struct chaos_inode dir_ino;
    if (chaos_inode_read(dir_inode_num, &dir_ino) != CHAOS_OK) return CHAOS_ERR_IO;

    size_t name_len = strlen(name);
    if (name_len > CHAOS_MAX_FILENAME) return CHAOS_ERR_TOO_LONG;

    uint8_t* buf = (uint8_t*)kmalloc(CHAOS_BLOCK_SIZE);
    if (!buf) return CHAOS_ERR_NO_SPACE;

    /* Search existing blocks for a free slot */
    uint32_t total = inode_total_blocks(&dir_ino);
    for (uint32_t b = 0; b < total; b++) {
        uint32_t phys = inode_logical_block(&dir_ino, b);
        if (phys == CHAOS_BLOCK_NULL) continue;
        if (chaos_block_read(phys, buf) != CHAOS_OK) continue;

        for (uint32_t s = 0; s < CHAOS_DIRENTS_PER_BLK; s++) {
            struct chaos_dirent* d = (struct chaos_dirent*)(buf + s * CHAOS_DIRENT_SIZE);
            if (d->inode == CHAOS_INODE_NULL) {
                /* Found free slot */
                memset(d, 0, CHAOS_DIRENT_SIZE);
                d->inode = target_inode;
                d->type = type;
                d->name_len = (uint8_t)name_len;
                strncpy(d->name, name, 53);
                d->name[53] = '\0';
                chaos_block_write(phys, buf);
                kfree(buf);
                return CHAOS_OK;
            }
        }
    }

    /* No free slot — allocate a new block for the directory */
    uint32_t new_block = chaos_alloc_block();
    if (new_block == CHAOS_BLOCK_NULL) { kfree(buf); return CHAOS_ERR_NO_SPACE; }

    memset(buf, 0, CHAOS_BLOCK_SIZE);
    struct chaos_dirent* d = (struct chaos_dirent*)buf;
    d->inode = target_inode;
    d->type = type;
    d->name_len = (uint8_t)name_len;
    strncpy(d->name, name, 53);
    d->name[53] = '\0';

    /* Write the new block */
    chaos_block_write(new_block, buf);
    kfree(buf);

    /* Add extent to directory inode */
    if (dir_ino.extent_count < CHAOS_MAX_INLINE_EXTENTS) {
        /* Try to extend last extent if contiguous */
        if (dir_ino.extent_count > 0) {
            struct chaos_extent* last = &dir_ino.extents[dir_ino.extent_count - 1];
            if (last->start_block + last->block_count == new_block) {
                last->block_count++;
                dir_ino.size += CHAOS_BLOCK_SIZE;
                return chaos_inode_write_through(dir_inode_num, &dir_ino);
            }
        }
        dir_ino.extents[dir_ino.extent_count].start_block = new_block;
        dir_ino.extents[dir_ino.extent_count].block_count = 1;
        dir_ino.extent_count++;
    } else {
        /* TODO: indirect extent block */
        chaos_free_block(new_block);
        return CHAOS_ERR_NO_SPACE;
    }

    dir_ino.size += CHAOS_BLOCK_SIZE;
    return chaos_inode_write_through(dir_inode_num, &dir_ino);
}

/* Remove a directory entry by name */
int chaos_dir_remove(uint32_t dir_inode_num, const char* name) {
    struct chaos_inode dir_ino;
    if (chaos_inode_read(dir_inode_num, &dir_ino) != CHAOS_OK) return CHAOS_ERR_IO;

    uint32_t blk_idx, slot;
    struct chaos_dirent found;
    if (chaos_dir_find(dir_inode_num, name, &found, &blk_idx, &slot) < 0) {
        return CHAOS_ERR_NOT_FOUND;
    }

    /* Clear the entry */
    uint32_t phys = inode_logical_block(&dir_ino, blk_idx);
    uint8_t* buf = (uint8_t*)kmalloc(CHAOS_BLOCK_SIZE);
    if (!buf) return CHAOS_ERR_IO;

    if (chaos_block_read(phys, buf) != CHAOS_OK) { kfree(buf); return CHAOS_ERR_IO; }

    struct chaos_dirent* d = (struct chaos_dirent*)(buf + slot * CHAOS_DIRENT_SIZE);
    memset(d, 0, CHAOS_DIRENT_SIZE);  /* inode = 0 marks slot as free */

    chaos_block_write(phys, buf);
    kfree(buf);
    return CHAOS_OK;
}

/* ── Path Resolution ───────────────────────────────── */

/* Split path into parent dir inode and last component name.
 * Returns parent inode number, writes last component to out_name.
 * Returns CHAOS_INODE_NULL on error. */
static uint32_t resolve_parent(const char* path, char* out_name, size_t name_size) {
    if (!path || path[0] != '/') return CHAOS_INODE_NULL;
    if (strlen(path) >= CHAOS_MAX_PATH) return CHAOS_INODE_NULL;

    /* Find last '/' */
    const char* last_slash = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    /* Extract name */
    const char* name = last_slash + 1;
    if (*name == '\0') return CHAOS_INODE_NULL;  /* trailing slash */
    strncpy(out_name, name, name_size - 1);
    out_name[name_size - 1] = '\0';

    /* Resolve parent path */
    if (last_slash == path) {
        /* Parent is root */
        return 1;
    }

    /* Build parent path */
    size_t parent_len = (size_t)(last_slash - path);
    char parent_path[CHAOS_MAX_PATH];
    memcpy(parent_path, path, parent_len);
    parent_path[parent_len] = '\0';

    return chaos_resolve_path(parent_path);
}

uint32_t chaos_resolve_path(const char* path) {
    if (!path || path[0] != '/') return CHAOS_INODE_NULL;
    if (strlen(path) >= CHAOS_MAX_PATH) return CHAOS_INODE_NULL;

    /* Root directory */
    if (strcmp(path, "/") == 0) return 1;

    uint32_t current = 1;  /* start at root (inode 1) */
    char component[CHAOS_MAX_FILENAME + 1];
    const char* p = path + 1;  /* skip leading '/' */
    int depth = 0;

    while (*p) {
        if (depth >= CHAOS_MAX_PATH_DEPTH) return CHAOS_INODE_NULL;

        /* Extract next component */
        const char* slash = p;
        while (*slash && *slash != '/') slash++;

        size_t len = (size_t)(slash - p);
        if (len == 0) { p = slash + 1; continue; }  /* skip double slashes */
        if (len > CHAOS_MAX_FILENAME) return CHAOS_INODE_NULL;

        memcpy(component, p, len);
        component[len] = '\0';

        /* Look up component in current directory */
        struct chaos_dirent entry;
        if (chaos_dir_find(current, component, &entry, NULL, NULL) < 0) {
            return CHAOS_INODE_NULL;
        }

        current = entry.inode;
        p = *slash ? slash + 1 : slash;
        depth++;
    }

    return current;
}

/* ── mkdir / rmdir ─────────────────────────────────── */

int chaos_mkdir(const char* path) {
    char name[CHAOS_MAX_FILENAME + 1];
    uint32_t parent = resolve_parent(path, name, sizeof(name));
    if (parent == CHAOS_INODE_NULL) return CHAOS_ERR_INVALID;

    /* Check if already exists */
    if (chaos_dir_find(parent, name, NULL, NULL, NULL) >= 0) {
        return CHAOS_ERR_EXISTS;
    }

    /* Allocate inode */
    uint32_t ino_num = chaos_alloc_inode();
    if (ino_num == CHAOS_INODE_NULL) return CHAOS_ERR_NO_INODES;

    /* Allocate data block for . and .. */
    uint32_t data_block = chaos_alloc_block();
    if (data_block == CHAOS_BLOCK_NULL) {
        chaos_free_inode(ino_num);
        return CHAOS_ERR_NO_SPACE;
    }

    /* Initialize directory data block with . and .. */
    uint8_t* buf = (uint8_t*)kzmalloc(CHAOS_BLOCK_SIZE);
    if (!buf) {
        chaos_free_block(data_block);
        chaos_free_inode(ino_num);
        return CHAOS_ERR_NO_SPACE;
    }

    struct chaos_dirent* dot = (struct chaos_dirent*)buf;
    dot->inode = ino_num;
    dot->type = CHAOS_DT_DIR;
    dot->name_len = 1;
    strcpy(dot->name, ".");

    struct chaos_dirent* dotdot = (struct chaos_dirent*)(buf + CHAOS_DIRENT_SIZE);
    dotdot->inode = parent;
    dotdot->type = CHAOS_DT_DIR;
    dotdot->name_len = 2;
    strcpy(dotdot->name, "..");

    /* Write data block first (write ordering: data before metadata) */
    chaos_block_write(data_block, buf);
    kfree(buf);

    /* Write inode (metadata: inode before directory entry) */
    struct chaos_inode ino;
    memset(&ino, 0, sizeof(ino));
    ino.magic = CHAOS_INODE_MAGIC;
    ino.mode = CHAOS_TYPE_DIR | 0755;
    ino.link_count = 2;  /* . entry + parent's entry */
    ino.size = CHAOS_BLOCK_SIZE;
    ino.extent_count = 1;
    ino.extents[0].start_block = data_block;
    ino.extents[0].block_count = 1;
    chaos_inode_write_through(ino_num, &ino);

    /* Add entry in parent directory (last step: dirent after inode) */
    int r = chaos_dir_add(parent, name, ino_num, CHAOS_DT_DIR);
    if (r != CHAOS_OK) {
        chaos_free_block(data_block);
        chaos_free_inode(ino_num);
        return r;
    }

    /* Increment parent's link_count (for .. in new dir) */
    struct chaos_inode parent_ino;
    chaos_inode_read(parent, &parent_ino);
    parent_ino.link_count++;
    chaos_inode_write(parent, &parent_ino);

    return CHAOS_OK;
}

int chaos_rmdir(const char* path) {
    char name[CHAOS_MAX_FILENAME + 1];
    uint32_t parent = resolve_parent(path, name, sizeof(name));
    if (parent == CHAOS_INODE_NULL) return CHAOS_ERR_INVALID;

    /* Find the directory */
    struct chaos_dirent entry;
    if (chaos_dir_find(parent, name, &entry, NULL, NULL) < 0) {
        return CHAOS_ERR_NOT_FOUND;
    }
    if (entry.type != CHAOS_DT_DIR) return CHAOS_ERR_NOT_DIR;

    /* Check if directory is empty (only . and ..) */
    struct chaos_inode dir_ino;
    if (chaos_inode_read(entry.inode, &dir_ino) != CHAOS_OK) return CHAOS_ERR_IO;

    uint32_t total = inode_total_blocks(&dir_ino);
    uint8_t* buf = (uint8_t*)kmalloc(CHAOS_BLOCK_SIZE);
    if (!buf) return CHAOS_ERR_IO;

    int entry_count = 0;
    for (uint32_t b = 0; b < total; b++) {
        uint32_t phys = inode_logical_block(&dir_ino, b);
        if (phys == CHAOS_BLOCK_NULL) continue;
        if (chaos_block_read(phys, buf) != CHAOS_OK) continue;

        for (uint32_t s = 0; s < CHAOS_DIRENTS_PER_BLK; s++) {
            struct chaos_dirent* d = (struct chaos_dirent*)(buf + s * CHAOS_DIRENT_SIZE);
            if (d->inode != CHAOS_INODE_NULL) entry_count++;
        }
    }
    kfree(buf);

    if (entry_count > 2) return CHAOS_ERR_NOT_EMPTY;  /* more than . and .. */

    /* Free directory data blocks */
    for (int i = 0; i < dir_ino.extent_count && i < CHAOS_MAX_INLINE_EXTENTS; i++) {
        for (uint32_t b = 0; b < dir_ino.extents[i].block_count; b++) {
            chaos_free_block(dir_ino.extents[i].start_block + b);
        }
    }

    /* Free inode */
    chaos_free_inode(entry.inode);

    /* Remove entry from parent */
    chaos_dir_remove(parent, name);

    /* Decrement parent link_count (removing ..) */
    struct chaos_inode parent_ino;
    chaos_inode_read(parent, &parent_ino);
    if (parent_ino.link_count > 0) parent_ino.link_count--;
    chaos_inode_write(parent, &parent_ino);

    return CHAOS_OK;
}

/* ── opendir / readdir / closedir ──────────────────── */

int chaos_opendir(const char* path) {
    uint32_t ino_num = chaos_resolve_path(path);
    if (ino_num == CHAOS_INODE_NULL) return CHAOS_ERR_NOT_FOUND;

    struct chaos_inode ino;
    if (chaos_inode_read(ino_num, &ino) != CHAOS_OK) return CHAOS_ERR_IO;
    if (!(ino.mode & CHAOS_TYPE_DIR)) return CHAOS_ERR_NOT_DIR;

    for (int i = 0; i < CHAOS_MAX_DIR_HANDLES; i++) {
        if (!dir_handles[i].in_use) {
            dir_handles[i].in_use = true;
            dir_handles[i].inode_num = ino_num;
            dir_handles[i].block_idx = 0;
            dir_handles[i].slot_idx = 0;
            return i;
        }
    }
    return CHAOS_ERR_NO_FD;
}

int chaos_readdir(int handle, struct chaos_dirent* out) {
    if (handle < 0 || handle >= CHAOS_MAX_DIR_HANDLES) return CHAOS_ERR_BAD_FD;
    if (!dir_handles[handle].in_use || !out) return CHAOS_ERR_BAD_FD;

    struct chaos_dir_handle* dh = &dir_handles[handle];
    struct chaos_inode dir_ino;
    if (chaos_inode_read(dh->inode_num, &dir_ino) != CHAOS_OK) return CHAOS_ERR_IO;

    uint32_t total = inode_total_blocks(&dir_ino);
    uint8_t* buf = (uint8_t*)kmalloc(CHAOS_BLOCK_SIZE);
    if (!buf) return CHAOS_ERR_IO;

    while (dh->block_idx < total) {
        uint32_t phys = inode_logical_block(&dir_ino, dh->block_idx);
        if (phys == CHAOS_BLOCK_NULL) {
            dh->block_idx++;
            dh->slot_idx = 0;
            continue;
        }

        if (chaos_block_read(phys, buf) != CHAOS_OK) {
            dh->block_idx++;
            dh->slot_idx = 0;
            continue;
        }

        while (dh->slot_idx < CHAOS_DIRENTS_PER_BLK) {
            struct chaos_dirent* d = (struct chaos_dirent*)(buf + dh->slot_idx * CHAOS_DIRENT_SIZE);
            dh->slot_idx++;

            if (d->inode != CHAOS_INODE_NULL) {
                *out = *d;
                kfree(buf);
                return CHAOS_OK;
            }
        }

        dh->block_idx++;
        dh->slot_idx = 0;
    }

    kfree(buf);
    return CHAOS_ERR_NOT_FOUND;  /* end of directory */
}

int chaos_closedir(int handle) {
    if (handle < 0 || handle >= CHAOS_MAX_DIR_HANDLES) return CHAOS_ERR_BAD_FD;
    if (!dir_handles[handle].in_use) return CHAOS_ERR_BAD_FD;
    dir_handles[handle].in_use = false;
    return CHAOS_OK;
}

/* ── Helper: unlink a directory entry's target inode ────── */
static void unlink_dirent_target(uint32_t parent, const char *name, struct chaos_dirent *de) {
    struct chaos_inode victim_ino;
    if (chaos_inode_read(de->inode, &victim_ino) != CHAOS_OK) return;
    chaos_dir_remove(parent, name);
    victim_ino.link_count--;
    if (victim_ino.link_count == 0 && victim_ino.open_count == 0) {
        for (int i = 0; i < victim_ino.extent_count && i < CHAOS_MAX_INLINE_EXTENTS; i++) {
            for (uint32_t b = 0; b < victim_ino.extents[i].block_count; b++) {
                chaos_free_block(victim_ino.extents[i].start_block + b);
            }
        }
        chaos_free_inode(de->inode);
    } else {
        victim_ino.unlink_pending = (victim_ino.link_count == 0);
        chaos_inode_write(de->inode, &victim_ino);
    }
}

/* ── Rename (supports cross-directory moves) ──────────── */

int chaos_rename(const char* old_path, const char* new_path) {
    char old_name[CHAOS_MAX_FILENAME + 1];
    char new_name[CHAOS_MAX_FILENAME + 1];
    uint32_t old_parent = resolve_parent(old_path, old_name, sizeof(old_name));
    uint32_t new_parent = resolve_parent(new_path, new_name, sizeof(new_name));

    if (old_parent == CHAOS_INODE_NULL || new_parent == CHAOS_INODE_NULL)
        return CHAOS_ERR_INVALID;

    /* Same parent + same name → no-op */
    if (old_parent == new_parent && strcmp(old_name, new_name) == 0)
        return CHAOS_OK;

    /* Find old entry */
    struct chaos_dirent old_entry;
    uint32_t blk_idx, slot;
    if (chaos_dir_find(old_parent, old_name, &old_entry, &blk_idx, &slot) < 0) {
        return CHAOS_ERR_NOT_FOUND;
    }

    /* Check if new name already exists in target directory */
    struct chaos_dirent existing;
    if (chaos_dir_find(new_parent, new_name, &existing, NULL, NULL) >= 0) {
        if (existing.type == CHAOS_DT_DIR) return CHAOS_ERR_NOT_EMPTY;
        unlink_dirent_target(new_parent, new_name, &existing);
    }

    if (old_parent == new_parent) {
        /* Same-directory rename: just update the name in-place */
        struct chaos_inode dir_ino;
        if (chaos_inode_read(old_parent, &dir_ino) != CHAOS_OK) return CHAOS_ERR_IO;

        uint32_t phys = inode_logical_block(&dir_ino, blk_idx);
        uint8_t* buf = (uint8_t*)kmalloc(CHAOS_BLOCK_SIZE);
        if (!buf) return CHAOS_ERR_IO;

        if (chaos_block_read(phys, buf) != CHAOS_OK) { kfree(buf); return CHAOS_ERR_IO; }

        struct chaos_dirent* d = (struct chaos_dirent*)(buf + slot * CHAOS_DIRENT_SIZE);
        memset(d->name, 0, 54);
        strncpy(d->name, new_name, 53);
        d->name[53] = '\0';
        d->name_len = (uint8_t)strlen(new_name);

        chaos_block_write(phys, buf);
        kfree(buf);
    } else {
        /* Cross-directory move: add to new parent, remove from old */
        int r = chaos_dir_add(new_parent, new_name, old_entry.inode, old_entry.type);
        if (r != CHAOS_OK) return r;

        chaos_dir_remove(old_parent, old_name);

        /* For directory moves, update the .. entry to point to new parent */
        if (old_entry.type == CHAOS_DT_DIR) {
            struct chaos_inode moved_ino;
            if (chaos_inode_read(old_entry.inode, &moved_ino) == CHAOS_OK) {
                /* Find .. entry and update its inode to new_parent */
                uint32_t total = inode_total_blocks(&moved_ino);
                uint8_t* buf = (uint8_t*)kmalloc(CHAOS_BLOCK_SIZE);
                if (buf) {
                    for (uint32_t b = 0; b < total; b++) {
                        uint32_t phys = inode_logical_block(&moved_ino, b);
                        if (phys == CHAOS_BLOCK_NULL) continue;
                        if (chaos_block_read(phys, buf) != CHAOS_OK) continue;
                        for (uint32_t s = 0; s < CHAOS_DIRENTS_PER_BLK; s++) {
                            struct chaos_dirent* d = (struct chaos_dirent*)(buf + s * CHAOS_DIRENT_SIZE);
                            if (d->inode != CHAOS_INODE_NULL && d->name_len == 2 &&
                                d->name[0] == '.' && d->name[1] == '.') {
                                d->inode = new_parent;
                                chaos_block_write(phys, buf);
                                goto dotdot_done;
                            }
                        }
                    }
                    dotdot_done:
                    kfree(buf);
                }
                /* Update link counts: old parent loses .., new parent gains .. */
                struct chaos_inode old_p_ino, new_p_ino;
                if (chaos_inode_read(old_parent, &old_p_ino) == CHAOS_OK) {
                    if (old_p_ino.link_count > 0) old_p_ino.link_count--;
                    chaos_inode_write(old_parent, &old_p_ino);
                }
                if (chaos_inode_read(new_parent, &new_p_ino) == CHAOS_OK) {
                    new_p_ino.link_count++;
                    chaos_inode_write(new_parent, &new_p_ino);
                }
            }
        }
    }

    return CHAOS_OK;
}

void chaos_dir_init(void) {
    memset(dir_handles, 0, sizeof(dir_handles));
}
