/* AIOS v2 — MIDI File Parser (Phase 12)
 * Parses SMF format 0/1 into event arrays for sequencing. */

#include "midi_parse.h"
#include "../../include/string.h"
#include "../../drivers/serial.h"

extern void *kmalloc(size_t size);
extern void  kfree(void *ptr);

static uint16_t be16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

/* Read variable-length quantity */
static uint32_t read_vlq(const uint8_t *data, uint32_t size, uint32_t *pos) {
    uint32_t val = 0;
    for (int i = 0; i < 4 && *pos < size; i++) {
        uint8_t b = data[(*pos)++];
        val = (val << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return val;
}

static int track_add_event(struct midi_track *t, struct midi_event *e) {
    if (t->event_count >= t->event_capacity) {
        uint32_t new_cap = t->event_capacity ? t->event_capacity * 2 : 256;
        struct midi_event *new_events = (struct midi_event *)kmalloc(new_cap * sizeof(struct midi_event));
        if (!new_events) return -1;
        if (t->events) {
            memcpy(new_events, t->events, t->event_count * sizeof(struct midi_event));
            kfree(t->events);
        }
        t->events = new_events;
        t->event_capacity = new_cap;
    }
    t->events[t->event_count++] = *e;
    return 0;
}

int midi_parse(const uint8_t *data, uint32_t size, struct midi_file *out) {
    if (!data || !out || size < 14) return -1;
    memset(out, 0, sizeof(*out));

    /* MThd header */
    if (data[0] != 'M' || data[1] != 'T' || data[2] != 'h' || data[3] != 'd')
        return -1;
    uint32_t hdr_len = be32(data + 4);
    if (hdr_len < 6 || 8 + hdr_len > size) return -1;

    out->format = be16(data + 8);
    out->num_tracks = be16(data + 10);
    out->ticks_per_quarter = be16(data + 12);

    if (out->num_tracks > MIDI_MAX_TRACKS) out->num_tracks = MIDI_MAX_TRACKS;

    serial_printf("[midi] format=%d tracks=%d tpq=%d\n",
                  out->format, out->num_tracks, out->ticks_per_quarter);

    uint32_t pos = 8 + hdr_len;

    /* Parse each track */
    for (int t = 0; t < out->num_tracks && pos + 8 <= size; t++) {
        if (data[pos] != 'M' || data[pos+1] != 'T' || data[pos+2] != 'r' || data[pos+3] != 'k') {
            serial_printf("[midi] bad track %d header at %u\n", t, pos);
            break;
        }
        uint32_t trk_len = be32(data + pos + 4);
        pos += 8;
        uint32_t trk_end = pos + trk_len;
        if (trk_end > size) trk_end = size;

        uint32_t abs_tick = 0;
        uint8_t running_status = 0;

        while (pos < trk_end) {
            uint32_t delta = read_vlq(data, trk_end, &pos);
            abs_tick += delta;

            if (pos >= trk_end) break;
            uint8_t byte = data[pos];

            struct midi_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.abs_tick = abs_tick;

            if (byte == 0xFF) {
                /* Meta event */
                pos++;
                if (pos >= trk_end) break;
                uint8_t meta_type = data[pos++];
                uint32_t meta_len = read_vlq(data, trk_end, &pos);

                if (meta_type == MIDI_META_TEMPO && meta_len >= 3 && pos + 3 <= trk_end) {
                    ev.status = MIDI_META;
                    ev.data1 = MIDI_META_TEMPO;
                    ev.tempo_usec = (data[pos] << 16) | (data[pos+1] << 8) | data[pos+2];
                    track_add_event(&out->tracks[t], &ev);
                } else if (meta_type == MIDI_META_END_TRACK) {
                    pos += meta_len;
                    break;
                }
                pos += meta_len;
            } else if (byte == 0xF0 || byte == 0xF7) {
                /* SysEx — skip */
                pos++;
                uint32_t sysex_len = read_vlq(data, trk_end, &pos);
                pos += sysex_len;
            } else {
                /* Channel message */
                if (byte & 0x80) {
                    running_status = byte;
                    pos++;
                }
                /* else running status applies */

                uint8_t type = running_status & 0xF0;
                ev.status = running_status;

                if (type == MIDI_NOTE_OFF || type == MIDI_NOTE_ON ||
                    type == MIDI_CONTROL || type == MIDI_PITCH_BEND) {
                    if (pos + 1 < trk_end) {
                        ev.data1 = data[pos++];
                        ev.data2 = data[pos++];
                        /* Note on with velocity 0 = note off */
                        if (type == MIDI_NOTE_ON && ev.data2 == 0)
                            ev.status = MIDI_NOTE_OFF | (running_status & 0x0F);
                        track_add_event(&out->tracks[t], &ev);
                    }
                } else if (type == MIDI_PROGRAM) {
                    if (pos < trk_end) {
                        ev.data1 = data[pos++];
                        track_add_event(&out->tracks[t], &ev);
                    }
                } else {
                    /* Unknown — skip 1 or 2 data bytes based on type */
                    if (type == 0xD0 || type == 0xC0) {
                        pos++;
                    } else {
                        pos += 2;
                    }
                }
            }
        }

        out->total_events += out->tracks[t].event_count;
        pos = trk_end;
    }

    serial_printf("[midi] parsed %u total events\n", out->total_events);
    return 0;
}

void midi_free(struct midi_file *mid) {
    if (!mid) return;
    for (int t = 0; t < MIDI_MAX_TRACKS; t++) {
        if (mid->tracks[t].events) {
            kfree(mid->tracks[t].events);
            mid->tracks[t].events = NULL;
        }
    }
}
