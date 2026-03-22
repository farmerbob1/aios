/* AIOS v2 — Audio Bridge + Mixer (Phase 12)
 *
 * Manages audio sources and mixes them in IRQ context.
 * Single-producer (decoder task) / single-consumer (IRQ fill callback). */

#include "audio_bridge.h"
#include "../../include/string.h"
#include "../../include/kaos/export.h"
#include "../../drivers/serial.h"

/* Kernel heap */
extern void *kmalloc(size_t size);
extern void  kfree(void *ptr);

static struct audio_source sources[AUDIO_MAX_SOURCES];
static int master_volume = 80;  /* 0-100 */
static bool driver_registered = false;
static audio_fill_fn driver_fill = NULL;
static audio_info_fn driver_info = NULL;
static int next_id = 1;

/* ── The fill callback registered with AC97 ────────────── */

static void audio_mixer_fill(int16_t *buf, uint32_t sample_count) {
    uint32_t total_values = sample_count * 2;  /* stereo */

    /* Zero output buffer */
    memset(buf, 0, total_values * sizeof(int16_t));

    /* Mix all active, non-paused sources */
    for (int i = 0; i < AUDIO_MAX_SOURCES; i++) {
        struct audio_source *s = &sources[i];
        if (!s->active || s->paused) continue;

        /* How many int16 values available in ring? */
        uint32_t wp = s->write_pos;
        uint32_t rp = s->read_pos;
        uint32_t avail;
        if (wp >= rp)
            avail = wp - rp;
        else
            avail = s->ring_size - rp + wp;

        uint32_t to_mix = (avail < total_values) ? avail : total_values;

        for (uint32_t j = 0; j < to_mix; j++) {
            int32_t sample = (int32_t)s->ring_buf[(rp + j) % s->ring_size];
            sample = (sample * s->volume) >> 8;

            /* Saturating add */
            int32_t mixed = (int32_t)buf[j] + sample;
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;
            buf[j] = (int16_t)mixed;
        }
        s->read_pos = (rp + to_mix) % s->ring_size;
    }

    /* Apply master volume */
    if (master_volume < 100) {
        for (uint32_t i = 0; i < total_values; i++) {
            buf[i] = (int16_t)(((int32_t)buf[i] * master_volume) / 100);
        }
    }
}

/* ── Init ──────────────────────────────────────────────── */

void audio_bridge_init(void) {
    memset(sources, 0, sizeof(sources));
    driver_registered = false;
    driver_fill = NULL;
    driver_info = NULL;
    master_volume = 80;
    next_id = 1;
    serial_print("[audio] bridge initialized\n");
}

/* ── Driver registration (called by AC97 module) ───────── */

int audio_bridge_register_driver(audio_fill_fn fill_cb, audio_info_fn info_cb) {
    driver_fill = fill_cb;
    driver_info = info_cb;
    driver_registered = true;

    /* Tell the driver to use our mixer as its fill callback */
    if (fill_cb) {
        /* The AC97 module passes a set_fill function pointer as fill_cb.
         * We call it with our mixer function. See ac97.c pattern. */
    }

    serial_print("[audio] driver registered\n");
    return 0;
}

bool audio_bridge_has_driver(void) {
    return driver_registered;
}

/* This is called by the AC97 module to get the mixer fill function */
audio_fill_fn audio_bridge_get_fill(void) {
    return audio_mixer_fill;
}

/* ── Source management ─────────────────────────────────── */

int audio_source_create(uint32_t ring_samples) {
    for (int i = 0; i < AUDIO_MAX_SOURCES; i++) {
        if (!sources[i].active) {
            uint32_t ring_values = ring_samples * 2;  /* stereo */
            int16_t *buf = (int16_t *)kmalloc(ring_values * sizeof(int16_t));
            if (!buf) return -1;

            memset(buf, 0, ring_values * sizeof(int16_t));
            memset(&sources[i], 0, sizeof(struct audio_source));
            sources[i].ring_buf = buf;
            sources[i].ring_size = ring_values;
            sources[i].read_pos = 0;
            sources[i].write_pos = 0;
            sources[i].volume = 256;  /* full volume */
            sources[i].active = true;
            sources[i].paused = false;
            sources[i].finished = false;
            sources[i].id = next_id++;

            serial_printf("[audio] source %d created (ring=%u samples)\n",
                          sources[i].id, ring_samples);
            return sources[i].id;
        }
    }
    return -1;
}

static struct audio_source *find_source(int id) {
    for (int i = 0; i < AUDIO_MAX_SOURCES; i++) {
        if (sources[i].active && sources[i].id == id)
            return &sources[i];
    }
    return NULL;
}

void audio_source_destroy(int id) {
    struct audio_source *s = find_source(id);
    if (!s) return;
    s->active = false;
    if (s->ring_buf) {
        kfree(s->ring_buf);
        s->ring_buf = NULL;
    }
}

int audio_source_write(int id, const int16_t *data, uint32_t count) {
    struct audio_source *s = find_source(id);
    if (!s || !s->active) return -1;

    uint32_t wp = s->write_pos;
    uint32_t rp = s->read_pos;
    uint32_t used;
    if (wp >= rp)
        used = wp - rp;
    else
        used = s->ring_size - rp + wp;

    uint32_t free_space = s->ring_size - used - 1;  /* -1 to distinguish full from empty */
    if (count > free_space) count = free_space;
    if (count == 0) return 0;

    for (uint32_t i = 0; i < count; i++) {
        s->ring_buf[(wp + i) % s->ring_size] = data[i];
    }
    s->write_pos = (wp + count) % s->ring_size;
    return (int)count;
}

int audio_source_available(int id) {
    struct audio_source *s = find_source(id);
    if (!s || !s->active) return 0;

    uint32_t wp = s->write_pos;
    uint32_t rp = s->read_pos;
    uint32_t used;
    if (wp >= rp)
        used = wp - rp;
    else
        used = s->ring_size - rp + wp;

    return (int)(s->ring_size - used - 1);
}

void audio_source_pause(int id) {
    struct audio_source *s = find_source(id);
    if (s) s->paused = true;
}

void audio_source_resume(int id) {
    struct audio_source *s = find_source(id);
    if (s) s->paused = false;
}

void audio_source_set_volume(int id, int vol) {
    struct audio_source *s = find_source(id);
    if (s) {
        if (vol < 0) vol = 0;
        if (vol > 256) vol = 256;
        s->volume = (int16_t)vol;
    }
}

bool audio_source_is_finished(int id) {
    struct audio_source *s = find_source(id);
    if (!s) return true;
    if (!s->finished) return false;
    /* Check if ring is also drained */
    return s->read_pos == s->write_pos;
}

bool audio_source_is_active(int id) {
    struct audio_source *s = find_source(id);
    return s && s->active;
}

void audio_source_mark_finished(int id) {
    struct audio_source *s = find_source(id);
    if (s) s->finished = true;
}

/* ── Master volume ─────────────────────────────────────── */

void audio_set_master_volume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    master_volume = vol;
}

int audio_get_master_volume(void) {
    return master_volume;
}

/* ── KAOS Exports ──────────────────────────────────────── */

KAOS_EXPORT(audio_bridge_register_driver)
KAOS_EXPORT(audio_bridge_has_driver)
KAOS_EXPORT(audio_bridge_get_fill)
KAOS_EXPORT(audio_source_create)
KAOS_EXPORT(audio_source_destroy)
KAOS_EXPORT(audio_source_write)
KAOS_EXPORT(audio_source_available)
KAOS_EXPORT(audio_source_pause)
KAOS_EXPORT(audio_source_resume)
KAOS_EXPORT(audio_source_set_volume)
KAOS_EXPORT(audio_source_is_finished)
KAOS_EXPORT(audio_source_is_active)
KAOS_EXPORT(audio_source_mark_finished)
KAOS_EXPORT(audio_set_master_volume)
KAOS_EXPORT(audio_get_master_volume)
