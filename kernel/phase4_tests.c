/* AIOS v2 — Phase 4 Acceptance Tests (ChaosFS)
 * Extracted from kernel/main.c to reduce code size per compilation unit. */

#include "../include/types.h"
#include "../include/string.h"
#include "../drivers/serial.h"
#include "heap.h"
#include "chaos/chaos.h"
#include "boot_display.h"
#include "phase4_tests.h"

static bool test_chaosfs_mount(void) {
    /* FS was already mounted at boot. Verify it's live. */
    if (chaos_total_blocks() == 0) {
        serial_printf("    (not mounted)\n");
        return false;
    }
    serial_printf("    (blocks=%u free=%u inodes_free=%u label='%s')\n",
                  chaos_total_blocks(), chaos_free_blocks(),
                  chaos_free_inodes(), chaos_label());
    return true;
}

static bool test_chaosfs_write_read(void) {
    int fd = chaos_open("/test.txt", CHAOS_O_CREAT | CHAOS_O_RDWR);
    if (fd < 0) { serial_printf("    (open failed: %d)\n", fd); return false; }

    const char* msg = "hello world";
    int w = chaos_write(fd, msg, 11);
    if (w != 11) { serial_printf("    (write=%d)\n", w); chaos_close(fd); return false; }

    chaos_seek(fd, 0, CHAOS_SEEK_SET);
    char buf[32] = {0};
    int r = chaos_read(fd, buf, 11);
    chaos_close(fd);

    if (r != 11 || memcmp(buf, "hello world", 11) != 0) {
        serial_printf("    (read=%d data='%s')\n", r, buf);
        return false;
    }

    /* Verify stat */
    struct chaos_stat st;
    if (chaos_stat("/test.txt", &st) != CHAOS_OK || st.size != 11) {
        serial_printf("    (stat failed or size=%u)\n", (uint32_t)st.size);
        return false;
    }

    chaos_unlink("/test.txt");
    return true;
}

static bool test_chaosfs_large_file(void) {
    int fd = chaos_open("/large.bin", CHAOS_O_CREAT | CHAOS_O_RDWR);
    if (fd < 0) return false;

    /* Write 64KB in 4KB chunks */
    uint8_t* chunk = (uint8_t*)kmalloc(4096);
    if (!chunk) { chaos_close(fd); return false; }

    for (int i = 0; i < 16; i++) {
        memset(chunk, (uint8_t)(i + 1), 4096);
        int w = chaos_write(fd, chunk, 4096);
        if (w != 4096) {
            serial_printf("    (write chunk %d failed: %d)\n", i, w);
            kfree(chunk); chaos_close(fd); return false;
        }
    }

    /* Read back and verify */
    chaos_seek(fd, 0, CHAOS_SEEK_SET);
    bool ok = true;
    for (int i = 0; i < 16; i++) {
        memset(chunk, 0, 4096);
        int r = chaos_read(fd, chunk, 4096);
        if (r != 4096) { ok = false; break; }
        for (int j = 0; j < 4096; j++) {
            if (chunk[j] != (uint8_t)(i + 1)) { ok = false; break; }
        }
        if (!ok) break;
    }

    struct chaos_stat st;
    chaos_fstat(fd, &st);
    serial_printf("    (size=%u blocks=%u)\n", (uint32_t)st.size, st.block_count);

    kfree(chunk);
    chaos_close(fd);
    chaos_unlink("/large.bin");
    return ok && st.size == 65536;
}

static bool test_chaosfs_directories(void) {
    int r = chaos_mkdir("/scripts");
    if (r != CHAOS_OK) { serial_printf("    (mkdir /scripts: %d)\n", r); return false; }

    r = chaos_mkdir("/scripts/ai");
    if (r != CHAOS_OK) { serial_printf("    (mkdir /scripts/ai: %d)\n", r); return false; }

    /* Create a file in the nested dir */
    int fd = chaos_open("/scripts/ai/test.lua", CHAOS_O_CREAT | CHAOS_O_RDWR);
    if (fd < 0) { serial_printf("    (create file: %d)\n", fd); return false; }
    chaos_write(fd, "print('hi')", 11);
    chaos_close(fd);

    /* rmdir should fail (not empty) */
    r = chaos_rmdir("/scripts/ai");
    if (r != CHAOS_ERR_NOT_EMPTY) { serial_printf("    (rmdir non-empty: %d)\n", r); return false; }

    /* Clean up */
    chaos_unlink("/scripts/ai/test.lua");
    r = chaos_rmdir("/scripts/ai");
    if (r != CHAOS_OK) { serial_printf("    (rmdir /scripts/ai: %d)\n", r); return false; }
    r = chaos_rmdir("/scripts");
    if (r != CHAOS_OK) { serial_printf("    (rmdir /scripts: %d)\n", r); return false; }

    return true;
}

static bool test_chaosfs_readdir(void) {
    /* Create some files */
    for (int i = 0; i < 5; i++) {
        char path[32];
        serial_printf("");  /* force format string usage */
        path[0] = '/'; path[1] = 'a' + (char)i; path[2] = '.'; path[3] = 't'; path[4] = 'x'; path[5] = 't'; path[6] = '\0';
        int fd = chaos_open(path, CHAOS_O_CREAT | CHAOS_O_WRONLY);
        if (fd >= 0) chaos_close(fd);
    }

    int dh = chaos_opendir("/");
    if (dh < 0) return false;

    int count = 0;
    struct chaos_dirent entry;
    while (chaos_readdir(dh, &entry) == CHAOS_OK) {
        count++;
    }
    chaos_closedir(dh);

    serial_printf("    (entries=%d, expected >=7: . .. + 5 files + optional dirs)\n", count);

    /* Clean up */
    for (int i = 0; i < 5; i++) {
        char path[32];
        path[0] = '/'; path[1] = 'a' + (char)i; path[2] = '.'; path[3] = 't'; path[4] = 'x'; path[5] = 't'; path[6] = '\0';
        chaos_unlink(path);
    }

    return count >= 7;
}

static bool test_chaosfs_unlink_reuse(void) {
    uint32_t free_before = chaos_free_blocks();

    int fd = chaos_open("/temp.txt", CHAOS_O_CREAT | CHAOS_O_RDWR);
    if (fd < 0) return false;
    chaos_write(fd, "data", 4);
    chaos_close(fd);

    chaos_unlink("/temp.txt");
    uint32_t free_after = chaos_free_blocks();

    /* Blocks should be freed */
    if (free_after != free_before) {
        serial_printf("    (free blocks: %u -> %u -> %u)\n", free_before, free_before, free_after);
        return false;
    }

    /* Path should resolve to NOT_FOUND */
    struct chaos_stat st;
    if (chaos_stat("/temp.txt", &st) != CHAOS_ERR_NOT_FOUND) return false;

    return true;
}

static bool test_chaosfs_seek(void) {
    int fd = chaos_open("/seek.txt", CHAOS_O_CREAT | CHAOS_O_RDWR);
    if (fd < 0) return false;

    chaos_write(fd, "ABCDEFGHIJ", 10);

    /* Seek SET 5, read 3 → "FGH" */
    chaos_seek(fd, 5, CHAOS_SEEK_SET);
    char buf[4] = {0};
    chaos_read(fd, buf, 3);
    if (memcmp(buf, "FGH", 3) != 0) {
        serial_printf("    (seek read: '%s')\n", buf);
        chaos_close(fd); chaos_unlink("/seek.txt");
        return false;
    }

    /* Seek END 0, write "XYZ" */
    chaos_seek(fd, 0, CHAOS_SEEK_END);
    chaos_write(fd, "XYZ", 3);

    /* Read full */
    chaos_seek(fd, 0, CHAOS_SEEK_SET);
    char full[16] = {0};
    int r = chaos_read(fd, full, 13);
    chaos_close(fd);
    chaos_unlink("/seek.txt");

    if (r != 13 || memcmp(full, "ABCDEFGHIJXYZ", 13) != 0) {
        serial_printf("    (full: '%s' r=%d)\n", full, r);
        return false;
    }

    return true;
}

static bool test_chaosfs_rename(void) {
    int fd = chaos_open("/a.txt", CHAOS_O_CREAT | CHAOS_O_RDWR);
    if (fd < 0) return false;
    chaos_write(fd, "hello", 5);
    chaos_close(fd);

    int r = chaos_rename("/a.txt", "/b.txt");
    if (r != CHAOS_OK) { serial_printf("    (rename: %d)\n", r); chaos_unlink("/a.txt"); return false; }

    /* Old path should be gone */
    struct chaos_stat st;
    if (chaos_stat("/a.txt", &st) != CHAOS_ERR_NOT_FOUND) {
        chaos_unlink("/a.txt"); chaos_unlink("/b.txt");
        return false;
    }

    /* New path should have data */
    fd = chaos_open("/b.txt", CHAOS_O_RDONLY);
    if (fd < 0) { chaos_unlink("/b.txt"); return false; }
    char buf[8] = {0};
    chaos_read(fd, buf, 5);
    chaos_close(fd);
    chaos_unlink("/b.txt");

    return memcmp(buf, "hello", 5) == 0;
}

static bool test_chaosfs_cross_dir_rename(void) {
    chaos_mkdir("/subdir");
    int fd = chaos_open("/a.txt", CHAOS_O_CREAT | CHAOS_O_WRONLY);
    if (fd >= 0) chaos_close(fd);

    int r = chaos_rename("/a.txt", "/subdir/a.txt");
    chaos_unlink("/a.txt");
    chaos_rmdir("/subdir");

    return r == CHAOS_ERR_INVALID;
}

static bool test_chaosfs_truncate(void) {
    int fd = chaos_open("/trunc.txt", CHAOS_O_CREAT | CHAOS_O_RDWR);
    if (fd < 0) return false;

    /* Write 100 bytes */
    uint8_t data[100];
    memset(data, 'A', 100);
    chaos_write(fd, data, 100);

    /* Expand to 8192 */
    int r = chaos_truncate(fd, 8192);
    if (r != CHAOS_OK) { chaos_close(fd); chaos_unlink("/trunc.txt"); return false; }

    struct chaos_stat st;
    chaos_fstat(fd, &st);
    if (st.size != 8192) { chaos_close(fd); chaos_unlink("/trunc.txt"); return false; }

    /* Read byte 100 — should be zero (gap filled) */
    chaos_seek(fd, 100, CHAOS_SEEK_SET);
    uint8_t b = 0xFF;
    chaos_read(fd, &b, 1);
    if (b != 0) {
        serial_printf("    (byte 100 = 0x%02x, expected 0)\n", b);
        chaos_close(fd); chaos_unlink("/trunc.txt");
        return false;
    }

    /* Shrink to 50 */
    chaos_truncate(fd, 50);
    chaos_fstat(fd, &st);

    chaos_close(fd);
    chaos_unlink("/trunc.txt");

    return st.size == 50;
}

static bool test_chaosfs_fd_exhaustion(void) {
    int fds[CHAOS_MAX_FD];
    int opened = 0;

    /* Create files and open them all */
    for (int i = 0; i < CHAOS_MAX_FD; i++) {
        char path[16];
        path[0] = '/'; path[1] = 'f';
        path[2] = '0' + (char)(i / 10); path[3] = '0' + (char)(i % 10);
        path[4] = '\0';
        fds[i] = chaos_open(path, CHAOS_O_CREAT | CHAOS_O_RDWR);
        if (fds[i] >= 0) opened++;
    }

    /* 17th should fail */
    int extra = chaos_open("/extra.txt", CHAOS_O_CREAT | CHAOS_O_RDWR);
    bool exhaustion_works = (extra == CHAOS_ERR_NO_FD);

    /* Close one, retry */
    if (opened > 0) chaos_close(fds[0]);
    extra = chaos_open("/extra.txt", CHAOS_O_CREAT | CHAOS_O_RDWR);
    bool reuse_works = (extra >= 0);
    if (extra >= 0) chaos_close(extra);

    /* Close all and clean up */
    for (int i = 1; i < CHAOS_MAX_FD; i++) {
        if (fds[i] >= 0) chaos_close(fds[i]);
    }
    for (int i = 0; i < CHAOS_MAX_FD; i++) {
        char path[16];
        path[0] = '/'; path[1] = 'f';
        path[2] = '0' + (char)(i / 10); path[3] = '0' + (char)(i % 10);
        path[4] = '\0';
        chaos_unlink(path);
    }
    chaos_unlink("/extra.txt");

    serial_printf("    (opened=%d exhaustion=%s reuse=%s)\n",
                  opened, exhaustion_works ? "yes" : "no", reuse_works ? "yes" : "no");

    return opened == CHAOS_MAX_FD && exhaustion_works && reuse_works;
}

static bool test_chaosfs_unlink_while_open(void) {
    uint32_t free_before = chaos_free_blocks();

    int fd = chaos_open("/open_unlink.txt", CHAOS_O_CREAT | CHAOS_O_RDWR);
    if (fd < 0) return false;
    chaos_write(fd, "still here", 10);

    /* Unlink while open */
    chaos_unlink("/open_unlink.txt");

    /* Path should be gone */
    struct chaos_stat st;
    if (chaos_stat("/open_unlink.txt", &st) != CHAOS_ERR_NOT_FOUND) {
        chaos_close(fd);
        return false;
    }

    /* FD should still read data */
    chaos_seek(fd, 0, CHAOS_SEEK_SET);
    char buf[16] = {0};
    int r = chaos_read(fd, buf, 10);
    if (r != 10 || memcmp(buf, "still here", 10) != 0) {
        serial_printf("    (read after unlink: r=%d)\n", r);
        chaos_close(fd);
        return false;
    }

    /* Close — should free blocks now */
    chaos_close(fd);
    uint32_t free_after = chaos_free_blocks();

    serial_printf("    (free: %u -> %u)\n", free_before, free_after);
    return free_after == free_before;
}

static bool test_chaosfs_fsck_clean(void) {
    chaos_sync();
    int errors = chaos_fsck();
    serial_printf("    (fsck errors=%d)\n", errors);
    return errors == 0;
}

void phase4_acceptance_tests(void) {
    if (!chaos_is_mounted()) {
        serial_print("\n=== Phase 4 Acceptance Tests ===\n");
        serial_print("  [SKIP] ChaosFS not mounted\n");
        return;
    }

    serial_print("\n=== Phase 4 Acceptance Tests ===\n");

    struct { const char* name; bool (*fn)(void); } tests[] = {
        { "Mount + verify",         test_chaosfs_mount },
        { "Basic write/read",       test_chaosfs_write_read },
        { "Large file (64KB)",      test_chaosfs_large_file },
        { "Directory creation",     test_chaosfs_directories },
        { "Directory listing",      test_chaosfs_readdir },
        { "Unlink + reuse",         test_chaosfs_unlink_reuse },
        { "Seek operations",        test_chaosfs_seek },
        { "Rename same-dir",        test_chaosfs_rename },
        { "Rename cross-dir reject",test_chaosfs_cross_dir_rename },
        { "Truncate expand/shrink", test_chaosfs_truncate },
        { "FD exhaustion",          test_chaosfs_fd_exhaustion },
        { "Unlink-while-open",      test_chaosfs_unlink_while_open },
        { "Clean fsck",             test_chaosfs_fsck_clean },
    };

    int count = sizeof(tests) / sizeof(tests[0]);
    int pass = 0, fail = 0;

    for (int i = 0; i < count; i++) {
        bool ok = tests[i].fn();
        serial_printf("  [%s] %s\n", ok ? "PASS" : "FAIL", tests[i].name);
        if (ok) pass++; else fail++;
    }

    serial_printf("\nPhase 4: %d/%d tests passed\n", pass, count);
    if (fail > 0) {
        serial_print("[AIOS v2] Phase 4 acceptance: FAIL\n");
    } else {
        serial_print("[AIOS v2] Phase 4 acceptance: PASS\n");
    }

    boot_print("\nAIOS v2 Phase 4 complete.\n");
}
