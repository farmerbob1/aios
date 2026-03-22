/* AIOS v2 — MIDI Renderer via TinySoundFont (Phase 12) */

#pragma once
#include "../../include/types.h"

struct midi_renderer;

/* Load SoundFont from memory. Returns renderer or NULL. */
struct midi_renderer *midi_renderer_create(const uint8_t *sf2_data, uint32_t sf2_size,
                                            int sample_rate);

/* Channel-based MIDI API (maps to TSF channels) */
void midi_renderer_channel_program(struct midi_renderer *r, int channel, int program);
void midi_renderer_channel_note_on(struct midi_renderer *r, int channel, int key, float vel);
void midi_renderer_channel_note_off(struct midi_renderer *r, int channel, int key);
void midi_renderer_channel_control(struct midi_renderer *r, int channel, int ctrl, int val);
void midi_renderer_note_off_all(struct midi_renderer *r);

/* Render samples to stereo int16 PCM. */
void midi_renderer_render(struct midi_renderer *r, int16_t *buf, int samples);

void midi_renderer_destroy(struct midi_renderer *r);
