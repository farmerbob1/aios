/* AIOS v2 — Audio Bridge (Phase 12)
 *
 * Decouples audio decoders (task context) from AC97 driver (IRQ context).
 * Mixes multiple audio sources into a single PCM output stream.
 * Pattern mirrors kernel/net/netif_bridge. */

#pragma once
#include "../../include/types.h"

#define AUDIO_MAX_SOURCES   16
#define AUDIO_SAMPLE_RATE   48000
#define AUDIO_CHANNELS      2
#define AUDIO_BITS          16

/* Fill callback: called from AC97 IRQ to fill next DMA buffer */
typedef void (*audio_fill_fn)(int16_t *buf, uint32_t sample_count);
/* Info callback: returns hardware capabilities */
typedef void (*audio_info_fn)(uint32_t *rate, uint32_t *channels, uint32_t *bits);

/* Audio source: one active decoder or sound effect */
struct audio_source {
    int16_t  *ring_buf;             /* Heap-allocated ring buffer (stereo interleaved) */
    uint32_t  ring_size;            /* Total int16 values in ring (samples * 2 for stereo) */
    volatile uint32_t read_pos;     /* Consumer (IRQ) reads from here */
    volatile uint32_t write_pos;    /* Producer (task) writes here */
    int16_t   volume;               /* Per-source volume: 0-256 (256 = 1.0) */
    bool      active;
    bool      paused;
    bool      finished;             /* Decoder has written all data */
    int       id;
};

void audio_bridge_init(void);

/* Called by AC97 driver module */
int  audio_bridge_register_driver(audio_fill_fn fill_cb, audio_info_fn info_cb);
bool audio_bridge_has_driver(void);

/* Called by decoders / Lua bindings */
int  audio_source_create(uint32_t ring_samples);    /* Returns source ID or -1 */
void audio_source_destroy(int id);
int  audio_source_write(int id, const int16_t *data, uint32_t count);  /* count = int16 values */
int  audio_source_available(int id);                /* Free int16 slots in ring */
void audio_source_pause(int id);
void audio_source_resume(int id);
void audio_source_set_volume(int id, int vol);      /* 0-256 */
bool audio_source_is_finished(int id);
bool audio_source_is_active(int id);
void audio_source_mark_finished(int id);

/* Master volume: 0-100 */
void audio_set_master_volume(int vol);
int  audio_get_master_volume(void);

/* For KAOS module registration */
typedef void (*audio_set_fill_fn)(audio_fill_fn cb);
