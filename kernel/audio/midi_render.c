/* AIOS v2 — MIDI Renderer via TinySoundFont (Phase 12)
 *
 * Compiled with TSF_CFLAGS (SSE2 enabled for float math).
 * Only called from task context where fxsave/fxrstor protects XMM state. */

#include "../../include/types.h"
#include "../../include/string.h"

extern void *kmalloc(size_t size);
extern void *krealloc(void *ptr, size_t size);
extern void  kfree(void *ptr);
extern void  serial_printf(const char *fmt, ...);
extern uint32_t pmm_alloc_pages(uint32_t count);
extern void     pmm_free_pages(uint32_t addr, uint32_t count);
extern void     vmm_map_range(uint32_t virt, uint32_t phys, uint32_t size, uint32_t flags);

#define PAGE_SIZE    4096
#define PTE_PRESENT  0x001
#define PTE_WRITABLE 0x002
#define LARGE_THRESHOLD (64 * 1024)  /* Use PMM for allocations > 64KB */

/* Track PMM allocations so we can free them */
#define MAX_PMM_ALLOCS 32
static struct { void *ptr; uint32_t pages; } pmm_allocs[MAX_PMM_ALLOCS];
static int pmm_alloc_count = 0;

static void *tsf_alloc(size_t sz) {
    if (sz >= LARGE_THRESHOLD) {
        uint32_t pages = (sz + PAGE_SIZE - 1) / PAGE_SIZE;
        uint32_t phys = pmm_alloc_pages(pages);
        if (!phys) return NULL;
        vmm_map_range(phys, phys, pages * PAGE_SIZE, PTE_PRESENT | PTE_WRITABLE);
        memset((void *)phys, 0, pages * PAGE_SIZE);
        if (pmm_alloc_count < MAX_PMM_ALLOCS) {
            pmm_allocs[pmm_alloc_count].ptr = (void *)phys;
            pmm_allocs[pmm_alloc_count].pages = pages;
            pmm_alloc_count++;
        }
        return (void *)phys;
    }
    return kmalloc(sz);
}

static void *tsf_realloc_fn(void *ptr, size_t sz) {
    /* Check if ptr is a PMM allocation */
    for (int i = 0; i < pmm_alloc_count; i++) {
        if (pmm_allocs[i].ptr == ptr) {
            /* Reallocating a PMM block — alloc new, copy, free old */
            void *new_ptr = tsf_alloc(sz);
            if (!new_ptr) return NULL;
            uint32_t old_size = pmm_allocs[i].pages * PAGE_SIZE;
            uint32_t copy_size = old_size < sz ? old_size : sz;
            memcpy(new_ptr, ptr, copy_size);
            pmm_free_pages((uint32_t)ptr, pmm_allocs[i].pages);
            /* Remove old entry */
            pmm_allocs[i] = pmm_allocs[pmm_alloc_count - 1];
            pmm_alloc_count--;
            return new_ptr;
        }
    }
    /* Small allocation — use krealloc, but check if result needs PMM */
    if (sz >= LARGE_THRESHOLD) {
        void *new_ptr = tsf_alloc(sz);
        if (!new_ptr) return NULL;
        if (ptr) {
            /* We don't know the old size, but krealloc's old data is there */
            /* Copy a safe amount — krealloc would have handled this, but we
             * can't mix kmalloc/pmm. Just copy up to sz. */
            memcpy(new_ptr, ptr, sz);  /* May over-read, but won't crash in identity-mapped kernel */
            kfree(ptr);
        }
        return new_ptr;
    }
    return krealloc(ptr, sz);
}

static void tsf_free_fn(void *ptr) {
    if (!ptr) return;
    for (int i = 0; i < pmm_alloc_count; i++) {
        if (pmm_allocs[i].ptr == ptr) {
            pmm_free_pages((uint32_t)ptr, pmm_allocs[i].pages);
            pmm_allocs[i] = pmm_allocs[pmm_alloc_count - 1];
            pmm_alloc_count--;
            return;
        }
    }
    kfree(ptr);
}

/* TSF configuration */
#define TSF_NO_STDIO
#define TSF_MALLOC(sz)       tsf_alloc(sz)
#define TSF_REALLOC(p, sz)   tsf_realloc_fn(p, sz)
#define TSF_FREE(p)          tsf_free_fn(p)
#define TSF_MEMCPY           memcpy
#define TSF_MEMSET           memset

#define TSF_IMPLEMENTATION
#include "tsf.h"

struct midi_renderer {
    tsf *synth;
};

struct midi_renderer *midi_renderer_create(const uint8_t *sf2_data, uint32_t sf2_size,
                                            int sample_rate) {
    struct midi_renderer *r = (struct midi_renderer *)kmalloc(sizeof(struct midi_renderer));
    if (!r) return NULL;

    r->synth = tsf_load_memory(sf2_data, (int)sf2_size);
    if (!r->synth) {
        serial_printf("[midi] failed to load SoundFont (%u bytes)\n", sf2_size);
        kfree(r);
        return NULL;
    }

    tsf_set_output(r->synth, TSF_STEREO_INTERLEAVED, sample_rate, 0.0f);
    serial_printf("[midi] SoundFont loaded (%u bytes), %d presets\n",
                  sf2_size, tsf_get_presetcount(r->synth));
    return r;
}

void midi_renderer_channel_program(struct midi_renderer *r, int channel, int program) {
    if (r && r->synth) {
        int is_drums = (channel == 9) ? 1 : 0;
        tsf_channel_set_presetnumber(r->synth, channel, program, is_drums);
    }
}

void midi_renderer_channel_note_on(struct midi_renderer *r, int channel, int key, float vel) {
    if (r && r->synth) tsf_channel_note_on(r->synth, channel, key, vel);
}

void midi_renderer_channel_note_off(struct midi_renderer *r, int channel, int key) {
    if (r && r->synth) tsf_channel_note_off(r->synth, channel, key);
}

void midi_renderer_channel_control(struct midi_renderer *r, int channel, int ctrl, int val) {
    if (r && r->synth) tsf_channel_midi_control(r->synth, channel, ctrl, val);
}

void midi_renderer_note_off_all(struct midi_renderer *r) {
    if (r && r->synth) tsf_note_off_all(r->synth);
}

void midi_renderer_render(struct midi_renderer *r, int16_t *buf, int samples) {
    if (r && r->synth) {
        tsf_render_short(r->synth, (short *)buf, samples, 0);
    }
}

void midi_renderer_destroy(struct midi_renderer *r) {
    if (!r) return;
    if (r->synth) tsf_close(r->synth);
    kfree(r);
}
