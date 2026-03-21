/* AIOS v2 — Phase 1 Acceptance Tests (Memory Subsystem)
 * Extracted from kernel/main.c to reduce code size per compilation unit. */

#include "../include/types.h"
#include "../include/string.h"
#include "../drivers/serial.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "phase1_tests.h"

static bool test_pmm_basic(void) {
    uint32_t initial_free = pmm_get_free_pages();
    uint32_t pages[1000];

    for (int i = 0; i < 1000; i++) {
        pages[i] = pmm_alloc_page();
        if (pages[i] == 0) return false;
    }

    if (pmm_get_free_pages() != initial_free - 1000) return false;

    for (int i = 0; i < 1000; i++) {
        pmm_free_page(pages[i]);
    }

    if (pmm_get_free_pages() != initial_free) return false;

    /* Alloc again should succeed */
    for (int i = 0; i < 1000; i++) {
        pages[i] = pmm_alloc_page();
        if (pages[i] == 0) return false;
    }
    for (int i = 0; i < 1000; i++) {
        pmm_free_page(pages[i]);
    }

    return true;
}

static bool test_pmm_stress(void) {
    uint32_t initial_free = pmm_get_free_pages();

    /* Alloc until OOM, but cap at 50000 to avoid taking forever */
    uint32_t count = 0;
    uint32_t cap = initial_free < 50000 ? initial_free : 50000;
    uint32_t* pages = (uint32_t*)kmalloc(cap * sizeof(uint32_t));
    if (!pages) return false;

    while (count < cap) {
        uint32_t p = pmm_alloc_page();
        if (p == 0) break;
        pages[count++] = p;
    }

    serial_printf("    (allocated %u pages)\n", count);

    /* Free all */
    for (uint32_t i = 0; i < count; i++) {
        pmm_free_page(pages[i]);
    }

    kfree(pages);

    bool ok = (pmm_get_free_pages() == initial_free);
    return ok;
}

static bool test_pmm_contiguous(void) {
    uint32_t p1 = pmm_alloc_pages(256);
    if (p1 == 0) return false;

    pmm_free_pages(p1, 256);

    uint32_t p2 = pmm_alloc_pages(256);
    if (p2 == 0) return false;

    pmm_free_pages(p2, 256);
    return true;
}

static bool test_vmm_mapping(void) {
    uint32_t phys = pmm_alloc_page();
    if (phys == 0) return false;

    vmm_map_page(phys, phys, PTE_PRESENT | PTE_WRITABLE);

    volatile uint32_t* ptr = (volatile uint32_t*)phys;
    *ptr = 0xDEADBEEF;
    bool ok = (*ptr == 0xDEADBEEF);

    vmm_unmap_page(phys);
    pmm_free_page(phys);
    return ok;
}

static bool test_vmm_flags(void) {
    uint32_t phys = pmm_alloc_page();
    if (phys == 0) return false;

    vmm_map_page(phys, phys, PTE_PRESENT | PTE_WRITABLE | PTE_NOCACHE);

    /* Verify the mapping works with NOCACHE flag set */
    volatile uint32_t* ptr = (volatile uint32_t*)phys;
    *ptr = 0xCAFEBABE;
    bool ok = (*ptr == 0xCAFEBABE);

    /* Change flags to add WRITETHROUGH */
    vmm_set_flags(phys, PTE_PRESENT | PTE_WRITABLE | PTE_NOCACHE | PTE_WRITETHROUGH);
    *ptr = 0x12345678;
    ok = ok && (*ptr == 0x12345678);

    vmm_unmap_page(phys);
    pmm_free_page(phys);
    return ok;
}

static bool test_slab_basic(void) {
    uint32_t initial_slab = heap_get_slab_used();
    /* Use heap for the pointer array to avoid stack overflow */
    void** ptrs = (void**)kmalloc(10000 * sizeof(void*));
    if (!ptrs) return false;

    for (int i = 0; i < 10000; i++) {
        ptrs[i] = kmalloc(32);
        if (!ptrs[i]) { kfree(ptrs); return false; }
        *(uint32_t*)ptrs[i] = (uint32_t)i;
    }

    for (int i = 0; i < 10000; i++) {
        if (*(uint32_t*)ptrs[i] != (uint32_t)i) { kfree(ptrs); return false; }
        kfree(ptrs[i]);
    }

    kfree(ptrs);
    if (heap_get_slab_used() != initial_slab) return false;
    return true;
}

/* Simple pseudo-random for stress tests (LCG) */
static uint32_t rng_state = 12345;
static uint32_t rng_next(void) {
    rng_state = rng_state * 1103515245 + 12345;
    return (rng_state >> 16) & 0x7FFF;
}

static bool test_slab_stress(void) {
    uint32_t initial_used = heap_get_used();
    /* Use heap for arrays to avoid stack overflow */
    void** ptrs = (void**)kmalloc(10000 * sizeof(void*));
    int* indices = (int*)kmalloc(10000 * sizeof(int));
    if (!ptrs || !indices) { if (ptrs) kfree(ptrs); if (indices) kfree(indices); return false; }

    rng_state = 42;
    for (int i = 0; i < 10000; i++) {
        size_t sz = 16 + (rng_next() % 2033);
        ptrs[i] = kmalloc(sz);
        if (!ptrs[i]) { kfree(ptrs); kfree(indices); return false; }
    }

    /* Free in pseudo-random order (Fisher-Yates shuffle) */
    for (int i = 0; i < 10000; i++) indices[i] = i;
    rng_state = 99;
    for (int i = 9999; i > 0; i--) {
        int j = rng_next() % (i + 1);
        int tmp = indices[i]; indices[i] = indices[j]; indices[j] = tmp;
    }

    for (int i = 0; i < 10000; i++) {
        kfree(ptrs[indices[i]]);
    }

    kfree(indices);
    kfree(ptrs);

    if (heap_get_used() != initial_used) return false;
    return true;
}

static bool test_buddy_basic(void) {
    void* p = kmalloc(64 * 1024); /* 64KB — goes to buddy */
    if (!p) return false;

    /* Write pattern */
    memset(p, 0xAB, 64 * 1024);
    if (((uint8_t*)p)[0] != 0xAB) return false;
    if (((uint8_t*)p)[65535] != 0xAB) return false;

    kfree(p);

    /* Alloc again should succeed */
    void* p2 = kmalloc(64 * 1024);
    if (!p2) return false;
    kfree(p2);
    return true;
}

static bool test_buddy_splitting(void) {
    /* Alloc 3 x 4KB blocks (minimum buddy size, goes through buddy via >2048 path) */
    void* a = kmalloc(4000); /* slightly under 4KB, but > 2048 -> buddy */
    void* b = kmalloc(4000);
    void* c = kmalloc(4000);
    if (!a || !b || !c) return false;

    kfree(b); /* free the middle one */

    void* d = kmalloc(4000); /* should reuse b's block */
    if (!d) return false;

    kfree(a);
    kfree(c);
    kfree(d);
    return true;
}

static bool test_kfree_routing(void) {
    /* Slab allocation (64 bytes) */
    void* slab_ptr = kmalloc(64);
    if (!slab_ptr) return false;
    uint32_t slab_page = (uint32_t)slab_ptr >> PAGE_SHIFT;
    if (page_ownership[slab_page] != PAGE_SLAB) return false;
    kfree(slab_ptr);

    /* Buddy allocation (8KB) */
    void* buddy_ptr = kmalloc(8192);
    if (!buddy_ptr) return false;
    uint32_t buddy_page = (uint32_t)buddy_ptr >> PAGE_SHIFT;
    if (page_ownership[buddy_page] != PAGE_BUDDY) return false;
    kfree(buddy_ptr);

    return true;
}

static bool test_kfree_reserved(void) {
    /* Allocate a page, mark it reserved, verify kfree would reject it.
     * We can't actually call kfree (it panics), so just verify the ownership. */
    uint32_t phys = pmm_alloc_page();
    if (phys == 0) return false;

    vmm_map_page(phys, phys, PTE_PRESENT | PTE_WRITABLE);
    heap_mark_reserved(phys, 1);

    uint32_t page_idx = phys >> PAGE_SHIFT;
    bool ok = (page_ownership[page_idx] == PAGE_RESERVED);

    /* Clean up: unmark and free (set back to UNUSED so we can free the PMM page) */
    page_ownership[page_idx] = PAGE_UNUSED;
    pmm_free_page(phys);

    return ok;
}

static bool test_krealloc(void) {
    char* p = (char*)kmalloc(32);
    if (!p) return false;
    memcpy(p, "HELLO", 6);

    /* Realloc to 128 (slab to slab) */
    p = (char*)krealloc(p, 128);
    if (!p) return false;
    if (memcmp(p, "HELLO", 6) != 0) return false;

    /* Realloc to 8KB (slab to buddy) */
    p = (char*)krealloc(p, 8192);
    if (!p) return false;
    if (memcmp(p, "HELLO", 6) != 0) return false;

    kfree(p);
    return true;
}

static bool test_kmalloc_aligned(void) {
    /* 512 bytes, 16-byte alignment */
    void* p1 = kmalloc_aligned(512, 16);
    if (!p1) return false;
    if ((uint32_t)p1 & 0xF) return false; /* not 16-byte aligned */

    /* Write pattern */
    memset(p1, 0xCC, 512);
    if (((uint8_t*)p1)[0] != 0xCC) return false;
    kfree_aligned(p1);

    /* PAGE_SIZE alignment */
    void* p2 = kmalloc_aligned(256, PAGE_SIZE);
    if (!p2) return false;
    if ((uint32_t)p2 & (PAGE_SIZE - 1)) return false; /* not page-aligned */
    kfree_aligned(p2);

    return true;
}

static bool test_page_ownership(void) {
    /* Verify page_ownership is not null and accessible */
    if (!page_ownership) return false;

    /* Verify at least some buddy pages exist */
    bool found_buddy = false;
    uint32_t total = pmm_get_max_phys_addr() / PAGE_SIZE;
    for (uint32_t i = 0; i < total && i < 100000; i++) {
        if (page_ownership[i] == PAGE_BUDDY) {
            found_buddy = true;
            break;
        }
    }
    if (!found_buddy) return false;

    return true;
}

static bool test_memory_accounting(void) {
    uint32_t used = heap_get_used();
    uint32_t free = heap_get_free();
    uint32_t total = used + free;

    if (total == 0) return false;

    uint32_t used_before = heap_get_used();
    void* p = kmalloc(4096);
    if (!p) return false;
    uint32_t used_after = heap_get_used();
    kfree(p);
    uint32_t used_final = heap_get_used();

    if (used_after <= used_before) return false;
    if (used_final != used_before) return false;

    return true;
}

void phase1_acceptance_tests(void) {
    serial_print("\n=== Phase 1 Acceptance Tests ===\n");

    struct { const char* name; bool (*fn)(void); } tests[] = {
        { "PMM basic",           test_pmm_basic },
        { "PMM stress",          test_pmm_stress },
        { "PMM contiguous",      test_pmm_contiguous },
        { "VMM mapping",         test_vmm_mapping },
        { "VMM flags",           test_vmm_flags },
        { "Slab basic",          test_slab_basic },
        { "Slab stress",         test_slab_stress },
        { "Buddy basic",         test_buddy_basic },
        { "Buddy splitting",     test_buddy_splitting },
        { "kfree routing",       test_kfree_routing },
        { "kfree reserved",      test_kfree_reserved },
        { "krealloc",            test_krealloc },
        { "kmalloc_aligned",     test_kmalloc_aligned },
        { "Page ownership",      test_page_ownership },
        { "Memory accounting",   test_memory_accounting },
    };

    int count = sizeof(tests) / sizeof(tests[0]);
    int pass = 0, fail = 0;

    for (int i = 0; i < count; i++) {
        bool ok = tests[i].fn();
        serial_printf("  [%s] %s\n", ok ? "PASS" : "FAIL", tests[i].name);
        if (ok) pass++; else fail++;
    }

    serial_printf("\nPhase 1: %d/%d tests passed\n", pass, count);
    if (fail > 0) {
        serial_print("[AIOS v2] Phase 1 acceptance: FAIL\n");
    } else {
        serial_print("[AIOS v2] Phase 1 acceptance: PASS\n");
    }
}
