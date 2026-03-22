/* AIOS v2 — Lua Audio Bindings (Phase 12)
 *
 * Provides aios.audio.* API for Lua scripts:
 *   aios.audio.play(path)          → play_id or nil, err
 *   aios.audio.stop(play_id)
 *   aios.audio.pause(play_id)
 *   aios.audio.resume(play_id)
 *   aios.audio.volume(level)       → set master volume 0-100
 *   aios.audio.status(play_id)     → "playing"/"paused"/"stopped"/nil
 *   aios.audio.playing()           → table of active play IDs
 */

#include "../audio/audio_bridge.h"
#include "../audio/wav_decode.h"
#include "../audio/resample.h"
#include "../audio/mp3_decode.h"
#include "../audio/midi_parse.h"
#include "../audio/midi_render.h"
#include "../../include/types.h"
#include "../../include/string.h"
#include "../../drivers/serial.h"

/* Lua headers */
#include "lua.h"
#include "lauxlib.h"

/* Kernel API */
extern void *kmalloc(size_t size);
extern void  kfree(void *ptr);
extern int   task_create(const char *name, void (*entry)(void), int priority);
extern void  task_sleep(uint32_t ms);
extern uint32_t timer_get_ticks(void);
extern uint32_t timer_get_frequency(void);

/* ChaosFS API */
extern int chaos_open(const char *path, int flags);
extern int chaos_close(int fd);
extern int chaos_read(int fd, void *buf, uint32_t len);
extern int chaos_stat(const char *path, void *st);

/* PMM for large allocations */
extern uint32_t pmm_alloc_pages(uint32_t count);
extern void     pmm_free_pages(uint32_t addr, uint32_t count);
extern void     vmm_map_range(uint32_t virt, uint32_t phys, uint32_t size, uint32_t flags);

#define CHAOS_O_RDONLY 0x01
#define PAGE_SIZE 4096
#define PTE_PRESENT  0x001
#define PTE_WRITABLE 0x002

/* ── Playback Context ──────────────────────────────────── */

#define MAX_PLAYBACKS 16
#define DECODE_CHUNK  8192   /* Bytes to read per chunk from ChaosFS */

enum audio_format {
    FMT_WAV,
    FMT_MP3,
    FMT_MID,
    FMT_UNKNOWN,
};

struct playback_ctx {
    bool     active;
    int      source_id;      /* audio_bridge source ID */
    int      task_id;        /* scheduler task ID */
    char     path[128];
    enum audio_format format;

    /* File data (loaded into memory for decoding) */
    uint8_t *file_data;
    uint32_t file_size;

    /* WAV-specific */
    struct wav_info wav;

    /* Decode state */
    volatile bool stop_requested;
    volatile bool decode_done;
};

static struct playback_ctx playbacks[MAX_PLAYBACKS];

/* Forward declare decode tasks */
static void wav_decode_task(void);
static void mp3_decode_task(void);
static void midi_decode_task(void);

/* Cached SoundFont (loaded once, shared by all MIDI playbacks) */
static uint8_t *cached_sf2_data = NULL;
static uint32_t cached_sf2_size = 0;

/* ── Helpers ───────────────────────────────────────────── */

static enum audio_format detect_format(const char *path) {
    int len = 0;
    while (path[len]) len++;
    if (len >= 4) {
        const char *ext = path + len - 4;
        if (ext[0] == '.' && (ext[1] == 'w' || ext[1] == 'W') &&
            (ext[2] == 'a' || ext[2] == 'A') && (ext[3] == 'v' || ext[3] == 'V'))
            return FMT_WAV;
        if (ext[0] == '.' && (ext[1] == 'm' || ext[1] == 'M') &&
            (ext[2] == 'p' || ext[2] == 'P') && ext[3] == '3')
            return FMT_MP3;
        if (ext[0] == '.' && (ext[1] == 'm' || ext[1] == 'M') &&
            (ext[2] == 'i' || ext[2] == 'I') && (ext[3] == 'd' || ext[3] == 'D'))
            return FMT_MID;
    }
    return FMT_UNKNOWN;
}

static int find_free_playback(void) {
    for (int i = 0; i < MAX_PLAYBACKS; i++) {
        if (!playbacks[i].active) return i;
    }
    return -1;
}

static struct playback_ctx *find_playback(int source_id) {
    for (int i = 0; i < MAX_PLAYBACKS; i++) {
        if (playbacks[i].active && playbacks[i].source_id == source_id)
            return &playbacks[i];
    }
    return NULL;
}

/* Task-local context: the decode task reads this to know which playback to process.
 * Since tasks run sequentially on init, we set this before task_create. */
static volatile int current_decode_idx = -1;

/* ── WAV Decode Task ───────────────────────────────────── */

static void wav_decode_task(void) {
    int idx = current_decode_idx;
    if (idx < 0 || idx >= MAX_PLAYBACKS) return;
    struct playback_ctx *ctx = &playbacks[idx];

    uint32_t src_rate = ctx->wav.sample_rate;
    uint32_t channels = ctx->wav.channels;
    uint32_t bits = ctx->wav.bits_per_sample;
    uint32_t data_off = ctx->wav.data_offset;
    uint32_t data_size = ctx->wav.data_size;
    uint32_t bytes_per_sample = bits / 8;
    uint32_t frame_size = channels * bytes_per_sample;

    serial_printf("[audio] WAV decode: %uHz %uch %ubit, %u bytes\n",
                  src_rate, channels, bits, data_size);

    /* Temp buffers for conversion */
    uint32_t chunk_frames = 2048;
    int16_t *stereo_buf = (int16_t *)kmalloc(chunk_frames * 2 * sizeof(int16_t));
    int16_t *resample_buf = NULL;
    bool need_resample = (src_rate != AUDIO_SAMPLE_RATE);
    if (need_resample) {
        uint32_t max_out = (chunk_frames * AUDIO_SAMPLE_RATE / src_rate) + 16;
        resample_buf = (int16_t *)kmalloc(max_out * 2 * sizeof(int16_t));
    }

    if (!stereo_buf) goto done;

    uint32_t pos = data_off;
    uint32_t end = data_off + data_size;
    if (end > ctx->file_size) end = ctx->file_size;

    while (pos < end && !ctx->stop_requested) {
        /* How many frames can we process this iteration? */
        uint32_t remaining_bytes = end - pos;
        uint32_t frames_available = remaining_bytes / frame_size;
        uint32_t frames = frames_available < chunk_frames ? frames_available : chunk_frames;
        if (frames == 0) break;

        const uint8_t *src = ctx->file_data + pos;

        /* Convert to stereo int16 */
        if (bits == 16 && channels == 2) {
            /* Already correct format — just copy */
            memcpy(stereo_buf, src, frames * 4);
        } else if (bits == 16 && channels == 1) {
            mono_to_stereo((const int16_t *)src, frames, stereo_buf, chunk_frames);
        } else if (bits == 8 && channels == 2) {
            pcm8_to_pcm16(src, stereo_buf, frames * 2);
        } else if (bits == 8 && channels == 1) {
            /* 8-bit mono: convert to 16-bit first, then stereo */
            int16_t *tmp = stereo_buf;  /* reuse buffer temporarily */
            pcm8_to_pcm16(src, tmp, frames);
            /* Now mono_to_stereo in-place won't work, so use the resample buf or process backwards */
            /* Simple approach: process each frame */
            for (int i = (int)frames - 1; i >= 0; i--) {
                stereo_buf[i * 2] = tmp[i];
                stereo_buf[i * 2 + 1] = tmp[i];
            }
        }

        pos += frames * frame_size;

        /* Resample if needed */
        int16_t *out_buf = stereo_buf;
        uint32_t out_frames = frames;

        if (need_resample && resample_buf) {
            uint32_t max_out = (frames * AUDIO_SAMPLE_RATE / src_rate) + 16;
            out_frames = resample_stereo(stereo_buf, frames, src_rate, AUDIO_SAMPLE_RATE,
                                         resample_buf, max_out);
            out_buf = resample_buf;
        }

        /* Write to audio source ring buffer */
        uint32_t total_values = out_frames * 2;  /* stereo int16 values */
        uint32_t written = 0;
        while (written < total_values && !ctx->stop_requested) {
            int avail = audio_source_available(ctx->source_id);
            if (avail <= 0) {
                task_sleep(5);
                continue;
            }
            uint32_t to_write = total_values - written;
            if (to_write > (uint32_t)avail) to_write = (uint32_t)avail;
            audio_source_write(ctx->source_id, out_buf + written, to_write);
            written += to_write;
        }
    }

done:
    if (stereo_buf) kfree(stereo_buf);
    if (resample_buf) kfree(resample_buf);

    /* Mark source as finished so mixer knows to stop when ring drains */
    audio_source_mark_finished(ctx->source_id);
    ctx->decode_done = true;
    serial_printf("[audio] decode done: %s\n", ctx->path);
}

/* ── MP3 Decode Task ───────────────────────────────────── */

static void mp3_decode_task(void) {
    int idx = current_decode_idx;
    if (idx < 0 || idx >= MAX_PLAYBACKS) return;
    struct playback_ctx *ctx = &playbacks[idx];

    struct mp3_decoder *dec = mp3_decoder_create(ctx->file_data, ctx->file_size);
    if (!dec) {
        serial_print("[audio] MP3 decoder create failed\n");
        goto done;
    }

    /* Temp buffers */
    int16_t *frame_pcm = (int16_t *)kmalloc(MP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    int16_t *stereo_buf = (int16_t *)kmalloc(1152 * 2 * sizeof(int16_t));
    int16_t *resample_buf = (int16_t *)kmalloc(2048 * 2 * sizeof(int16_t));

    if (!frame_pcm || !stereo_buf || !resample_buf) {
        serial_print("[audio] MP3 decode alloc failed\n");
        if (frame_pcm) kfree(frame_pcm);
        if (stereo_buf) kfree(stereo_buf);
        if (resample_buf) kfree(resample_buf);
        mp3_decoder_destroy(dec);
        goto done;
    }

    bool first_frame = true;
    int src_rate = 44100;

    while (!ctx->stop_requested) {
        int channels = 0, hz = 0;
        int samples = mp3_decoder_decode_frame(dec, frame_pcm, &channels, &hz);
        if (samples == 0) {
            /* Could be skip or EOF */
            if (mp3_decoder_eof(dec)) break;
            continue;
        }

        if (first_frame && hz > 0) {
            src_rate = hz;
            serial_printf("[audio] MP3: %dHz %dch\n", hz, channels);
            first_frame = false;
        }

        /* Convert to stereo if mono */
        int16_t *src = frame_pcm;
        uint32_t frames = (uint32_t)samples;

        if (channels == 1) {
            mono_to_stereo(frame_pcm, frames, stereo_buf, 1152);
            src = stereo_buf;
        }

        /* Resample to 48kHz if needed */
        int16_t *out_buf = src;
        uint32_t out_frames = frames;

        if ((uint32_t)src_rate != AUDIO_SAMPLE_RATE && src_rate > 0) {
            uint32_t max_out = (frames * AUDIO_SAMPLE_RATE / (uint32_t)src_rate) + 16;
            if (max_out > 2048) max_out = 2048;
            out_frames = resample_stereo(src, frames, (uint32_t)src_rate, AUDIO_SAMPLE_RATE,
                                          resample_buf, max_out);
            out_buf = resample_buf;
        }

        /* Write to ring buffer */
        uint32_t total_values = out_frames * 2;
        uint32_t written = 0;
        while (written < total_values && !ctx->stop_requested) {
            int avail = audio_source_available(ctx->source_id);
            if (avail <= 0) {
                task_sleep(5);
                continue;
            }
            uint32_t to_write = total_values - written;
            if (to_write > (uint32_t)avail) to_write = (uint32_t)avail;
            audio_source_write(ctx->source_id, out_buf + written, to_write);
            written += to_write;
        }
    }

    kfree(frame_pcm);
    kfree(stereo_buf);
    kfree(resample_buf);
    mp3_decoder_destroy(dec);

done:
    audio_source_mark_finished(ctx->source_id);
    ctx->decode_done = true;
    serial_printf("[audio] MP3 decode done: %s\n", ctx->path);
}

/* ── MIDI Decode Task ──────────────────────────────────── */

/* chaos_stat struct (matches chaos_types.h) */
struct sf2_stat {
    uint32_t inode;
    uint16_t mode;
    uint64_t size;
    uint32_t block_count;
    uint32_t created_time;
    uint32_t modified_time;
};

static int load_soundfont(void) {
    if (cached_sf2_data) return 0;  /* Already loaded */

    const char *paths[] = {
        "/sounds/gm.sf2",
        "/sounds/soundfont.sf2",
        "/system/audio/gm.sf2",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        /* Get file size first */
        struct sf2_stat st;
        if (chaos_stat(paths[i], &st) < 0) continue;
        uint32_t file_size = (uint32_t)st.size;
        if (file_size == 0) continue;

        serial_printf("[audio] SoundFont: %s (%u bytes)\n", paths[i], file_size);

        int fd = chaos_open(paths[i], CHAOS_O_RDONLY);
        if (fd < 0) continue;

        /* Allocate via PMM for large files (avoids heap fragmentation) */
        uint32_t pages = (file_size + PAGE_SIZE - 1) / PAGE_SIZE;
        uint32_t phys = pmm_alloc_pages(pages);
        if (!phys) {
            serial_printf("[audio] failed to alloc %u pages for SoundFont\n", pages);
            chaos_close(fd);
            continue;
        }
        vmm_map_range(phys, phys, pages * PAGE_SIZE, PTE_PRESENT | PTE_WRITABLE);
        uint8_t *data = (uint8_t *)phys;

        /* Read in chunks */
        uint32_t total = 0;
        while (total < file_size) {
            uint32_t chunk = file_size - total;
            if (chunk > 32768) chunk = 32768;
            int n = chaos_read(fd, data + total, chunk);
            if (n <= 0) break;
            total += (uint32_t)n;
        }
        chaos_close(fd);

        if (total == file_size) {
            cached_sf2_data = data;
            cached_sf2_size = total;
            serial_printf("[audio] SoundFont loaded: %s (%u bytes)\n", paths[i], total);
            return 0;
        }

        serial_printf("[audio] SoundFont read incomplete: %u/%u bytes\n", total, file_size);
        pmm_free_pages(phys, pages);
    }

    serial_print("[audio] no SoundFont file found\n");
    return -1;
}

static void midi_decode_task(void) {
    int idx = current_decode_idx;
    if (idx < 0 || idx >= MAX_PLAYBACKS) return;
    struct playback_ctx *ctx = &playbacks[idx];

    /* Load SoundFont if not cached */
    if (load_soundfont() < 0) goto done;

    /* Parse MIDI file */
    struct midi_file mid;
    if (midi_parse(ctx->file_data, ctx->file_size, &mid) < 0) {
        serial_print("[audio] MIDI parse failed\n");
        goto done;
    }

    /* Create renderer */
    struct midi_renderer *renderer = midi_renderer_create(cached_sf2_data, cached_sf2_size,
                                                           AUDIO_SAMPLE_RATE);
    if (!renderer) {
        midi_free(&mid);
        goto done;
    }

    /* Flatten all tracks into a single sorted event list */
    uint32_t total_events = mid.total_events;
    struct midi_event *all_events = (struct midi_event *)kmalloc(total_events * sizeof(struct midi_event));
    if (!all_events) {
        midi_renderer_destroy(renderer);
        midi_free(&mid);
        goto done;
    }

    uint32_t ei = 0;
    for (int t = 0; t < mid.num_tracks; t++) {
        for (uint32_t e = 0; e < mid.tracks[t].event_count; e++) {
            all_events[ei++] = mid.tracks[t].events[e];
        }
    }

    /* Simple insertion sort by abs_tick (events are mostly sorted already) */
    for (uint32_t i = 1; i < ei; i++) {
        struct midi_event tmp = all_events[i];
        int j = (int)i - 1;
        while (j >= 0 && all_events[j].abs_tick > tmp.abs_tick) {
            all_events[j + 1] = all_events[j];
            j--;
        }
        all_events[j + 1] = tmp;
    }

    uint32_t tpq = mid.ticks_per_quarter;
    if (tpq == 0) tpq = 480;
    midi_free(&mid);  /* Free per-track arrays, events are in all_events now */

    serial_printf("[audio] MIDI sequencer: %u events, tpq=%u\n", ei, tpq);

    /* Sequencer loop */
    uint32_t tempo_usec = 500000;  /* Default: 120 BPM */
    uint32_t event_idx = 0;
    uint32_t current_tick = 0;
    int16_t *render_buf = (int16_t *)kmalloc(512 * 2 * sizeof(int16_t));

    if (!render_buf) {
        kfree(all_events);
        midi_renderer_destroy(renderer);
        goto done;
    }

    /* How many audio samples per MIDI tick?
     * tempo_usec = microseconds per quarter note
     * tpq = ticks per quarter note
     * usec_per_tick = tempo_usec / tpq
     * samples_per_tick = (sample_rate * usec_per_tick) / 1000000 */

    while (!ctx->stop_requested) {
        /* Process all events at current tick */
        while (event_idx < ei && all_events[event_idx].abs_tick <= current_tick) {
            struct midi_event *ev = &all_events[event_idx];
            uint8_t type = ev->status & 0xF0;
            uint8_t ch = ev->status & 0x0F;

            if (ev->status == MIDI_META && ev->data1 == MIDI_META_TEMPO) {
                tempo_usec = ev->tempo_usec;
            } else if (type == MIDI_NOTE_ON) {
                midi_renderer_channel_note_on(renderer, ch, ev->data1, ev->data2 / 127.0f);
            } else if (type == MIDI_NOTE_OFF) {
                midi_renderer_channel_note_off(renderer, ch, ev->data1);
            } else if (type == MIDI_PROGRAM) {
                midi_renderer_channel_program(renderer, ch, ev->data1);
            } else if (type == MIDI_CONTROL) {
                midi_renderer_channel_control(renderer, ch, ev->data1, ev->data2);
            }
            event_idx++;
        }

        if (event_idx >= ei) break;  /* All events processed */

        /* Render audio until next event */
        uint32_t next_tick = all_events[event_idx].abs_tick;
        uint32_t delta_ticks = next_tick - current_tick;

        /* Calculate samples for this delta */
        uint64_t usec = (uint64_t)delta_ticks * tempo_usec / tpq;
        uint32_t samples = (uint32_t)((uint64_t)AUDIO_SAMPLE_RATE * usec / 1000000);
        if (samples == 0) samples = 1;

        /* Render in chunks of 512 samples */
        uint32_t rendered = 0;
        while (rendered < samples && !ctx->stop_requested) {
            uint32_t chunk = samples - rendered;
            if (chunk > 512) chunk = 512;

            midi_renderer_render(renderer, render_buf, (int)chunk);

            /* Write to audio source ring */
            uint32_t total_values = chunk * 2;
            uint32_t written = 0;
            while (written < total_values && !ctx->stop_requested) {
                int avail = audio_source_available(ctx->source_id);
                if (avail <= 0) {
                    task_sleep(5);
                    continue;
                }
                uint32_t to_write = total_values - written;
                if (to_write > (uint32_t)avail) to_write = (uint32_t)avail;
                audio_source_write(ctx->source_id, render_buf + written, to_write);
                written += to_write;
            }
            rendered += chunk;
        }

        current_tick = next_tick;
    }

    kfree(render_buf);
    kfree(all_events);
    midi_renderer_destroy(renderer);

done:
    audio_source_mark_finished(ctx->source_id);
    ctx->decode_done = true;
    serial_printf("[audio] MIDI decode done: %s\n", ctx->path);
}

/* ── Lua: aios.audio.play(path) ────────────────────────── */

static int l_audio_play(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);

    enum audio_format fmt = detect_format(path);
    if (fmt == FMT_UNKNOWN) {
        lua_pushnil(L);
        lua_pushstring(L, "unsupported format");
        return 2;
    }
    if (fmt == FMT_UNKNOWN) {
        lua_pushnil(L);
        lua_pushstring(L, "unsupported audio format");
        return 2;
    }

    /* Find free playback slot */
    int idx = find_free_playback();
    if (idx < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "too many active playbacks");
        return 2;
    }

    /* Read file into memory */
    /* First, get file size by reading stat */
    /* ChaosFS doesn't have a great stat API from kernel, so read in chunks */
    int fd = chaos_open(path, CHAOS_O_RDONLY);
    if (fd < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "file not found");
        return 2;
    }

    /* Read file in chunks to determine size and load data */
    uint32_t alloc_size = 256 * 1024;  /* Start with 256KB */
    uint8_t *data = (uint8_t *)kmalloc(alloc_size);
    if (!data) {
        chaos_close(fd);
        lua_pushnil(L);
        lua_pushstring(L, "out of memory");
        return 2;
    }

    uint32_t total = 0;
    while (1) {
        if (total >= alloc_size) {
            /* Grow buffer */
            uint32_t new_size = alloc_size * 2;
            uint8_t *new_data = (uint8_t *)kmalloc(new_size);
            if (!new_data) break;
            memcpy(new_data, data, total);
            kfree(data);
            data = new_data;
            alloc_size = new_size;
        }
        int n = chaos_read(fd, data + total, alloc_size - total);
        if (n <= 0) break;
        total += (uint32_t)n;
    }
    chaos_close(fd);

    if (total == 0) {
        kfree(data);
        lua_pushnil(L);
        lua_pushstring(L, "empty file");
        return 2;
    }

    /* Parse based on format */
    struct playback_ctx *ctx = &playbacks[idx];
    memset(ctx, 0, sizeof(*ctx));
    ctx->file_data = data;
    ctx->file_size = total;
    ctx->format = fmt;
    ctx->stop_requested = false;
    ctx->decode_done = false;

    /* Copy path */
    int plen = 0;
    while (path[plen] && plen < 127) { ctx->path[plen] = path[plen]; plen++; }
    ctx->path[plen] = 0;

    if (fmt == FMT_WAV) {
        if (wav_parse_header(data, total, &ctx->wav) < 0) {
            kfree(data);
            lua_pushnil(L);
            lua_pushstring(L, "invalid WAV file");
            return 2;
        }
    }

    /* Create audio source (ring = ~1 second of 48kHz stereo) */
    int src_id = audio_source_create(AUDIO_SAMPLE_RATE);
    if (src_id < 0) {
        kfree(data);
        lua_pushnil(L);
        lua_pushstring(L, "no free audio sources");
        return 2;
    }
    ctx->source_id = src_id;
    ctx->active = true;

    /* Spawn decoder task */
    current_decode_idx = idx;
    if (fmt == FMT_WAV) {
        ctx->task_id = task_create("wav_decode", wav_decode_task, 1);
    } else if (fmt == FMT_MP3) {
        ctx->task_id = task_create("mp3_decode", mp3_decode_task, 1);
    } else if (fmt == FMT_MID) {
        ctx->task_id = task_create("midi_decode", midi_decode_task, 1);
    }

    serial_printf("[audio] play: %s (source=%d)\n", path, src_id);
    lua_pushinteger(L, src_id);
    return 1;
}

/* ── Lua: aios.audio.stop(play_id) ────────────────────── */

static int l_audio_stop(lua_State *L) {
    int id = (int)luaL_checkinteger(L, 1);
    struct playback_ctx *ctx = find_playback(id);
    if (ctx) {
        ctx->stop_requested = true;
        /* Wait briefly for decode task to notice */
        for (int i = 0; i < 10 && !ctx->decode_done; i++)
            task_sleep(5);
        audio_source_destroy(id);
        if (ctx->file_data) kfree(ctx->file_data);
        ctx->file_data = NULL;
        ctx->active = false;
    }
    return 0;
}

/* ── Lua: aios.audio.pause(play_id) ───────────────────── */

static int l_audio_pause(lua_State *L) {
    int id = (int)luaL_checkinteger(L, 1);
    audio_source_pause(id);
    return 0;
}

/* ── Lua: aios.audio.resume(play_id) ──────────────────── */

static int l_audio_resume(lua_State *L) {
    int id = (int)luaL_checkinteger(L, 1);
    audio_source_resume(id);
    return 0;
}

/* ── Lua: aios.audio.volume(level) or volume(id, level) ── */

static int l_audio_volume(lua_State *L) {
    int nargs = lua_gettop(L);
    if (nargs >= 2) {
        /* Per-source volume: volume(play_id, level) */
        int id = (int)luaL_checkinteger(L, 1);
        int level = (int)luaL_checkinteger(L, 2);
        /* Convert 0-100 to 0-256 */
        int vol256 = (level * 256) / 100;
        audio_source_set_volume(id, vol256);
    } else {
        /* Master volume: volume(level) */
        int level = (int)luaL_checkinteger(L, 1);
        audio_set_master_volume(level);
    }
    return 0;
}

/* ── Lua: aios.audio.status(play_id) ──────────────────── */

static int l_audio_status(lua_State *L) {
    int id = (int)luaL_checkinteger(L, 1);
    struct playback_ctx *ctx = find_playback(id);
    if (!ctx) {
        lua_pushnil(L);
        return 1;
    }

    /* Check if finished (decode done + ring drained) */
    if (audio_source_is_finished(id)) {
        lua_pushstring(L, "stopped");
        /* Auto-cleanup */
        if (ctx->file_data) kfree(ctx->file_data);
        ctx->file_data = NULL;
        audio_source_destroy(id);
        ctx->active = false;
        return 1;
    }

    /* Check paused */
    /* We don't have a direct query, but we can track it */
    lua_pushstring(L, "playing");
    return 1;
}

/* ── Lua: aios.audio.playing() ─────────────────────────── */

static int l_audio_playing(lua_State *L) {
    lua_newtable(L);
    int n = 0;
    for (int i = 0; i < MAX_PLAYBACKS; i++) {
        if (playbacks[i].active) {
            n++;
            lua_pushinteger(L, playbacks[i].source_id);
            lua_rawseti(L, -2, n);
        }
    }
    return 1;
}

/* ── Registration ──────────────────────────────────────── */

static const luaL_Reg audio_funcs[] = {
    {"play",    l_audio_play},
    {"stop",    l_audio_stop},
    {"pause",   l_audio_pause},
    {"resume",  l_audio_resume},
    {"volume",  l_audio_volume},
    {"status",  l_audio_status},
    {"playing", l_audio_playing},
    {NULL, NULL}
};

void aios_register_audio(lua_State *L) {
    lua_getglobal(L, "aios");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "aios");
    }
    lua_newtable(L);
    luaL_setfuncs(L, audio_funcs, 0);
    lua_setfield(L, -2, "audio");
    lua_pop(L, 1);
}
