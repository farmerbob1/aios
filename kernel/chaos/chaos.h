/* AIOS v2 — ChaosFS Public API (Phase 4)
 * Single include for all filesystem operations. */

#pragma once

#include "chaos_types.h"

/* ── Lifecycle ─────────────────────────────────────── */

int  chaos_format(uint32_t lba_start, uint32_t lba_count, const char* label);
int  chaos_mount(uint32_t lba_start);
void chaos_unmount(void);
int  chaos_fsck(void);
void chaos_sync(void);

/* ── File operations ───────────────────────────────── */

int     chaos_open(const char* path, int flags);
int     chaos_close(int fd);
int     chaos_read(int fd, void* buf, uint32_t len);
int     chaos_write(int fd, const void* buf, uint32_t len);
int64_t chaos_seek(int fd, int64_t offset, int whence);
int     chaos_truncate(int fd, uint64_t size);
int     chaos_unlink(const char* path);
int     chaos_rename(const char* old_path, const char* new_path);

/* ── Stat ──────────────────────────────────────────── */

int chaos_stat(const char* path, struct chaos_stat* st);
int chaos_fstat(int fd, struct chaos_stat* st);

/* ── Directory operations ──────────────────────────── */

int chaos_mkdir(const char* path);
int chaos_rmdir(const char* path);
int chaos_opendir(const char* path);
int chaos_readdir(int handle, struct chaos_dirent* out);
int chaos_closedir(int handle);

/* ── Checksum ──────────────────────────────────────── */

uint32_t chaos_crc32(const void* data, size_t len);

/* ── Utility ───────────────────────────────────────── */

uint32_t    chaos_free_blocks(void);
uint32_t    chaos_free_inodes(void);
uint32_t    chaos_total_blocks(void);
const char* chaos_label(void);

/* ── Internal (used by fsck) ───────────────────────── */

bool chaos_is_mounted(void);
struct chaos_superblock* chaos_get_superblock(void);
