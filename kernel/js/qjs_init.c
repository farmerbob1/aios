/* AIOS QuickJS Initialization and Self-Test
 * Runs at boot after Lua init to verify QuickJS works. */

#include "../../include/types.h"
#include "../../include/string.h"
#include "../heap.h"
#include "../../drivers/serial.h"

#include "quickjs.h"

/* ── Custom allocator using kernel heap ──────────────── */
/* Each allocation is prefixed with a 16-byte header storing the size.
 * This lets free/realloc/usable_size work correctly. 16-byte alignment
 * ensures the returned pointer stays aligned. */

typedef struct {
    uint32_t size;
    uint32_t magic;     /* 0xBEEF — sanity check */
    uint32_t pad[2];    /* align to 16 bytes */
} qjs_alloc_hdr_t;

#define QJS_ALLOC_MAGIC 0xBEEFCAFE

static void *qjs_aios_malloc(JSMallocState *s, size_t size) {
    size_t total = sizeof(qjs_alloc_hdr_t) + size;
    if (s->malloc_size + size > s->malloc_limit)
        return NULL;
    qjs_alloc_hdr_t *hdr = (qjs_alloc_hdr_t *)kmalloc((uint32_t)total);
    if (!hdr) return NULL;
    hdr->size = (uint32_t)size;
    hdr->magic = QJS_ALLOC_MAGIC;
    s->malloc_size += size;
    return (void *)(hdr + 1);
}

static void qjs_aios_free(JSMallocState *s, void *ptr) {
    if (!ptr) return;
    qjs_alloc_hdr_t *hdr = ((qjs_alloc_hdr_t *)ptr) - 1;
    if (hdr->magic != QJS_ALLOC_MAGIC) {
        serial_printf("[qjs] BAD FREE: %p (magic=%x)\n", ptr, hdr->magic);
        return;
    }
    s->malloc_size -= hdr->size;
    hdr->magic = 0;  /* poison */
    kfree(hdr);
}

static void *qjs_aios_realloc(JSMallocState *s, void *ptr, size_t size) {
    if (!ptr)
        return qjs_aios_malloc(s, size);
    if (size == 0) {
        qjs_aios_free(s, ptr);
        return NULL;
    }
    qjs_alloc_hdr_t *old_hdr = ((qjs_alloc_hdr_t *)ptr) - 1;
    if (old_hdr->magic != QJS_ALLOC_MAGIC) {
        serial_printf("[qjs] BAD REALLOC: %p (magic=%x)\n", ptr, old_hdr->magic);
        return NULL;
    }
    uint32_t old_size = old_hdr->size;
    size_t new_total = sizeof(qjs_alloc_hdr_t) + size;
    if (s->malloc_size - old_size + size > s->malloc_limit)
        return NULL;
    qjs_alloc_hdr_t *new_hdr = (qjs_alloc_hdr_t *)krealloc(old_hdr, (uint32_t)new_total);
    if (!new_hdr) return NULL;
    s->malloc_size = s->malloc_size - old_size + size;
    new_hdr->size = (uint32_t)size;
    new_hdr->magic = QJS_ALLOC_MAGIC;
    return (void *)(new_hdr + 1);
}

static size_t qjs_aios_malloc_usable_size(const void *ptr) {
    if (!ptr) return 0;
    const qjs_alloc_hdr_t *hdr = ((const qjs_alloc_hdr_t *)ptr) - 1;
    if (hdr->magic != QJS_ALLOC_MAGIC) return 0;
    return hdr->size;
}

const JSMallocFunctions qjs_aios_mf = {
    .js_malloc = qjs_aios_malloc,
    .js_free = qjs_aios_free,
    .js_realloc = qjs_aios_realloc,
    .js_malloc_usable_size = qjs_aios_malloc_usable_size,
};

/* ── Self-test ───────────────────────────────────────── */

int qjs_init(void) {
    JSRuntime *rt = JS_NewRuntime2(&qjs_aios_mf, NULL);
    if (!rt) {
        serial_printf("[qjs] FAIL: could not create runtime\n");
        return -1;
    }
    JS_SetMemoryLimit(rt, 4 * 1024 * 1024);  /* 4MB for self-test */
    JS_SetMaxStackSize(rt, 256 * 1024);

    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) {
        serial_printf("[qjs] FAIL: could not create context\n");
        JS_FreeRuntime(rt);
        return -1;
    }

    /* Test 1: basic arithmetic */
    JSValue result = JS_Eval(ctx, "2 + 2", 5, "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        serial_printf("[qjs] FAIL: eval exception\n");
        JS_FreeValue(ctx, result);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return -1;
    }

    int32_t v;
    if (JS_ToInt32(ctx, &v, result) || v != 4) {
        serial_printf("[qjs] FAIL: 2+2 = %d (expected 4)\n", v);
        JS_FreeValue(ctx, result);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return -1;
    }
    JS_FreeValue(ctx, result);

    /* Test 2: string operations */
    const char *test2 = "'hello' + ' ' + 'world'";
    result = JS_Eval(ctx, test2, strlen(test2), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(result)) {
        const char *str = JS_ToCString(ctx, result);
        if (str) {
            if (strcmp(str, "hello world") != 0) {
                serial_printf("[qjs] WARN: string test got '%s'\n", str);
            }
            JS_FreeCString(ctx, str);
        }
    }
    JS_FreeValue(ctx, result);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    serial_printf("[qjs] QuickJS 2025-09-13 ready\n");
    return 0;
}
