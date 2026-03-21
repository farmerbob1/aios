/* kernel/phase6_tests.c — Phase 6 KAOS acceptance tests */

#include "../drivers/serial.h"
#include "../include/string.h"
#include "pmm.h"
#include "kaos/kaos.h"
#include "phase6_tests.h"

static int p6_pass, p6_total;

static void p6_check(const char* name, int cond) {
    p6_total++;
    if (cond) {
        p6_pass++;
        serial_printf("  [PASS] %s\n", name);
    } else {
        serial_printf("  [FAIL] %s\n", name);
    }
}

void phase6_acceptance_tests(void) {
    serial_printf("\n=== Phase 6 Acceptance Tests ===\n");
    p6_pass = 0;
    p6_total = 0;

    /* Test 1: Symbol table */
    {
        uint32_t count = kaos_sym_count();
        uint32_t kmalloc_addr = kaos_sym_lookup("kmalloc");
        uint32_t bad_addr = kaos_sym_lookup("nonexistent_symbol_xyz");
        p6_check("1. sym_count > 0", count > 0);
        p6_check("1b. lookup(kmalloc) != 0", kmalloc_addr != 0);
        p6_check("1c. lookup(nonexistent) == 0", bad_addr == 0);
    }

    /* Test 2: Load hello module */
    {
        /* First unload any auto-loaded hello */
        int existing = kaos_find("hello");
        if (existing >= 0) kaos_unload(existing);

        int idx = kaos_load("/modules/hello.kaos");
        const struct kaos_module* mod = (idx >= 0) ? kaos_get(idx) : 0;
        p6_check("2. load hello.kaos", idx >= 0);
        p6_check("2b. state == LOADED",
                 mod && mod->state == KAOS_STATE_LOADED);
        p6_check("2c. find(hello) valid", kaos_find("hello") >= 0);

        /* Test 3: Unload hello module */
        if (idx >= 0) {
            int rc = kaos_unload(idx);
            const struct kaos_module* mod2 = kaos_get(idx);
            p6_check("3. unload hello", rc == 0);
            p6_check("3b. state == UNLOADED",
                     mod2 && mod2->state == KAOS_STATE_UNLOADED);
        } else {
            p6_check("3. unload hello (skipped)", 0);
            p6_check("3b. state == UNLOADED (skipped)", 0);
        }
    }

    /* Test 4: Module calls kernel API */
    {
        int idx = kaos_load("/modules/api_test.kaos");
        p6_check("4. api_test module loaded + called kernel API", idx >= 0);
        if (idx >= 0) kaos_unload(idx);
    }

    /* Test 5: Reject corrupt file */
    {
        int idx = kaos_load("/modules/corrupt.kaos");
        p6_check("5. reject corrupt ELF", idx < 0);
    }

    /* Test 6: Reject ABI mismatch */
    {
        int idx = kaos_load("/modules/bad_abi.kaos");
        p6_check("6. reject ABI mismatch", idx < 0);
    }

    /* Test 7: Reject unresolved symbol */
    {
        int idx = kaos_load("/modules/unresolved.kaos");
        p6_check("7. reject unresolved symbol", idx < 0);
    }

    /* Test 8: Dependency resolution */
    {
        /* Make sure dep_b isn't already loaded */
        int existing_b = kaos_find("dep_b");
        if (existing_b >= 0) {
            int existing_a = kaos_find("dep_a");
            if (existing_a >= 0) kaos_unload(existing_a);
            kaos_unload(existing_b);
        }

        int idx_a = kaos_load("/modules/dep_a.kaos");
        int idx_b = kaos_find("dep_b");
        p6_check("8. dep_a loaded (auto-loads dep_b)", idx_a >= 0 && idx_b >= 0);

        /* Try to unload dep_b while dep_a depends on it */
        if (idx_b >= 0) {
            int rc = kaos_unload(idx_b);
            p6_check("8b. cannot unload dep_b (dep_a depends)", rc < 0);
        } else {
            p6_check("8b. cannot unload dep_b (skipped)", 0);
        }

        /* Clean up */
        if (idx_a >= 0) kaos_unload(idx_a);
        idx_b = kaos_find("dep_b");
        if (idx_b >= 0) kaos_unload(idx_b);
    }

    /* Test 9: Essential module */
    {
        int idx = kaos_load("/modules/essential.kaos");
        p6_check("9. essential module loaded", idx >= 0);
        if (idx >= 0) {
            int rc = kaos_unload(idx);
            p6_check("9b. cannot unload essential", rc < 0);
        } else {
            p6_check("9b. cannot unload essential (skipped)", 0);
        }
    }

    /* Test 10: Init failure */
    {
        int idx = kaos_load("/modules/fail_init.kaos");
        p6_check("10. init failure returns -1", idx < 0);
    }

    /* Test 11: Memory cleanup */
    {
        uint32_t before = pmm_get_free_pages();
        int idx = kaos_load("/modules/hello.kaos");
        if (idx >= 0) kaos_unload(idx);
        uint32_t after = pmm_get_free_pages();
        p6_check("11. PMM pages restored after load+unload", before == after);
    }

    /* Test 12: Module listing */
    {
        /* Clean state — unload everything possible */
        for (int i = 0; i < KAOS_MAX_MODULES; i++) {
            const struct kaos_module* m = kaos_get(i);
            if (m && m->state == KAOS_STATE_LOADED &&
                !(m->info && (m->info->flags & KAOS_FLAG_ESSENTIAL))) {
                kaos_unload(i);
            }
        }

        int i1 = kaos_load("/modules/hello.kaos");
        int i2 = kaos_load("/modules/api_test.kaos");
        int i3 = kaos_load("/modules/dep_b.kaos");
        int count = kaos_get_count();
        /* essential is still loaded, so count includes it */
        int non_essential_count = 0;
        if (i1 >= 0) non_essential_count++;
        if (i2 >= 0) non_essential_count++;
        if (i3 >= 0) non_essential_count++;
        p6_check("12. module listing count", count >= non_essential_count);

        /* Clean up */
        if (i1 >= 0) kaos_unload(i1);
        if (i2 >= 0) kaos_unload(i2);
        if (i3 >= 0) kaos_unload(i3);
    }

    /* Test 13: Auto-load */
    {
        /* Clean everything first */
        for (int i = 0; i < KAOS_MAX_MODULES; i++) {
            const struct kaos_module* m = kaos_get(i);
            if (m && m->state == KAOS_STATE_LOADED &&
                !(m->info && (m->info->flags & KAOS_FLAG_ESSENTIAL))) {
                kaos_unload(i);
            }
        }

        kaos_load_all("/modules/");
        int hello_idx = kaos_find("hello");
        p6_check("13. auto-load finds hello (AUTOLOAD)", hello_idx >= 0);

        /* Clean up all loaded modules */
        for (int i = 0; i < KAOS_MAX_MODULES; i++) {
            const struct kaos_module* m = kaos_get(i);
            if (m && m->state == KAOS_STATE_LOADED &&
                !(m->info && (m->info->flags & KAOS_FLAG_ESSENTIAL))) {
                kaos_unload(i);
            }
        }
    }

    /* Test 14: Lua binding — SKIP (Phase 7) */
    serial_printf("  [SKIP] 14. Lua binding (Phase 7)\n");

    /* Test 15: Max modules */
    {
        /* Load hello.kaos repeatedly until table is full */
        int loaded_count = 0;
        int last_rc = 0;
        for (int i = 0; i < KAOS_MAX_MODULES + 1; i++) {
            int rc = kaos_load("/modules/hello.kaos");
            if (rc >= 0) {
                loaded_count++;
            } else {
                last_rc = rc;
                break;
            }
        }
        p6_check("15. max modules (table full returns -1)",
                 loaded_count <= KAOS_MAX_MODULES && last_rc < 0);

        /* Clean up */
        for (int i = 0; i < KAOS_MAX_MODULES; i++) {
            const struct kaos_module* m = kaos_get(i);
            if (m && m->state == KAOS_STATE_LOADED) {
                kaos_unload(i);
            }
        }
    }

    serial_printf("Phase 6: %d/%d tests passed\n", p6_pass, p6_total);
}
