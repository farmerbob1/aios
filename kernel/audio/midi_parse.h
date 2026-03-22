/* AIOS v2 — MIDI File Parser (Phase 12)
 * Parses Standard MIDI File (SMF) format 0 and 1. */

#pragma once
#include "../../include/types.h"

#define MIDI_MAX_TRACKS 16

/* MIDI event types */
#define MIDI_NOTE_OFF       0x80
#define MIDI_NOTE_ON        0x90
#define MIDI_CONTROL        0xB0
#define MIDI_PROGRAM        0xC0
#define MIDI_PITCH_BEND     0xE0
#define MIDI_META           0xFF
#define MIDI_META_TEMPO     0x51
#define MIDI_META_END_TRACK 0x2F

struct midi_event {
    uint32_t abs_tick;      /* Absolute tick position */
    uint8_t  status;        /* Status byte (type + channel) */
    uint8_t  data1;         /* First data byte */
    uint8_t  data2;         /* Second data byte (or 0) */
    /* For tempo events */
    uint32_t tempo_usec;    /* Microseconds per quarter note */
};

struct midi_track {
    struct midi_event *events;
    uint32_t event_count;
    uint32_t event_capacity;
};

struct midi_file {
    uint16_t format;
    uint16_t num_tracks;
    uint16_t ticks_per_quarter;
    struct midi_track tracks[MIDI_MAX_TRACKS];
    uint32_t total_events;  /* Sum across all tracks */
};

/* Parse a MIDI file from memory. Returns 0 on success, -1 on error. */
int midi_parse(const uint8_t *data, uint32_t size, struct midi_file *out);

/* Free parsed MIDI data */
void midi_free(struct midi_file *mid);
