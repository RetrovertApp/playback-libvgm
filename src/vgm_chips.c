///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// VGM Chip Handlers - Implementation
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "vgm_chips.h"
#include "vgm_alloc.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define VGM_CHIPS_DEBUG 0

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Note name lookup table

static const char* s_note_names[12] = { "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-" };

static char s_note_buffer[8]; // Static buffer for note name formatting

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Event buffer implementation

VgmEventBuffer vgm_event_buffer_create(VgmAllocator* alloc, u32 capacity) {
    VgmEventBuffer buffer = { 0 };
    if (capacity > 0) {
        buffer.events = (VgmNoteEvent*)vgm_alloc_array(alloc, capacity * sizeof(VgmNoteEvent));
        buffer.capacity = capacity;
    }
    return buffer;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool vgm_event_buffer_push(VgmEventBuffer* buffer, const VgmNoteEvent* event) {
    if (buffer->count >= buffer->capacity) {
        return false;
    }
    buffer->events[buffer->count++] = *event;
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void vgm_event_buffer_clear(VgmEventBuffer* buffer) {
    buffer->count = 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Note name formatting

const char* vgm_note_name(u8 midi_note) {
    if (midi_note > 127) {
        return "---";
    }

    u8 note_in_octave = midi_note % 12;
    u8 octave = midi_note / 12;

    // Format as "C-4", "F#5", etc.
    s_note_buffer[0] = s_note_names[note_in_octave][0];
    s_note_buffer[1] = s_note_names[note_in_octave][1];
    s_note_buffer[2] = '0' + (octave % 10);
    s_note_buffer[3] = '\0';

    return s_note_buffer;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Channel naming - uses static string literals to avoid buffer overwrite issues

static const char* s_fm_channel_names[] = { "FM 1", "FM 2", "FM 3", "FM 4", "FM 5", "FM 6", "FM 7", "FM 8" };
static const char* s_psg_channel_names[] = { "PSG 1", "PSG 2", "PSG 3", "Noise" };
static const char* s_ay_channel_names[] = { "AY A", "AY B", "AY C" };

const char* vgm_channel_name(VgmChipId chip_id, u8 channel) {
    switch (chip_id) {
        case VGM_CHIP_YM2612:
        case VGM_CHIP_YM2151:
        case VGM_CHIP_YM2203:
        case VGM_CHIP_YM2608:
        case VGM_CHIP_YM2610:
            if (channel < 8) {
                return s_fm_channel_names[channel];
            }
            return "FM ?";

        case VGM_CHIP_SN76489:
            if (channel < 4) {
                return s_psg_channel_names[channel];
            }
            return "PSG ?";

        case VGM_CHIP_AY8910:
            if (channel < 3) {
                return s_ay_channel_names[channel];
            }
            return "AY ?";

        case VGM_CHIP_GAMEBOY:
            switch (channel) {
                case 0:
                    return "Pulse 1";
                case 1:
                    return "Pulse 2";
                case 2:
                    return "Wave";
                case 3:
                    return "Noise";
                default:
                    return "???";
            }

        case VGM_CHIP_NESAPU:
            switch (channel) {
                case 0:
                    return "Pulse 1";
                case 1:
                    return "Pulse 2";
                case 2:
                    return "Triangle";
                case 3:
                    return "Noise";
                case 4:
                    return "DMC";
                default:
                    return "???";
            }

        default: {
            static const char* s_generic_channel_names[]
                = { "CH 1", "CH 2",  "CH 3",  "CH 4",  "CH 5",  "CH 6",  "CH 7",  "CH 8",
                    "CH 9", "CH 10", "CH 11", "CH 12", "CH 13", "CH 14", "CH 15", "CH 16" };
            if (channel < 16) {
                return s_generic_channel_names[channel];
            }
            return "CH ?";
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// YM2612 Handler
//
// The YM2612 is a 6-channel FM synthesis chip used in the Sega Genesis/Mega Drive.
// Key registers:
// - 0xA0-0xA2 (port 0): Frequency low byte for channels 1-3
// - 0xA4-0xA6 (port 0): Frequency high + block for channels 1-3
// - 0xA0-0xA2 (port 1): Frequency low byte for channels 4-6
// - 0xA4-0xA6 (port 1): Frequency high + block for channels 4-6
// - 0x28: Key on/off register
// - 0x40-0x4F: Total Level (volume per operator)
// - 0xB0-0xB2: Algorithm + feedback
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct Ym2612Channel {
    u16 fnum;          // 11-bit frequency number
    u8 block;          // 3-bit octave/block
    u8 key_state;      // Bit mask of keyed-on operators (0-3)
    u8 total_level[4]; // Total level per operator (0-127)
    u8 algorithm;      // Algorithm (0-7)
    u8 last_note;      // Last emitted MIDI note (to filter duplicate NOTE_CHANGE)
    bool active;       // Is channel currently producing sound
} Ym2612Channel;

typedef struct Ym2612State {
    Ym2612Channel channels[6];
    u32 clock;
    u8 pending_fnum_high[6]; // Pending high byte writes (write high before low)
} Ym2612State;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

u8 vgm_ym2612_freq_to_note(u16 fnum, u8 block) {
    if (fnum == 0) {
        return 0;
    }

    // YM2612 frequency formula:
    // F = (fnum * clock) / (144 * 2^(21 - block))
    // For clock = 7670453:
    // F = fnum * 7670453 / (144 * 2^(21 - block))

    // We can approximate MIDI note from fnum and block directly.
    // Base frequency for fnum at block 4 with standard clock is around A4.
    // MIDI note = 12 * log2(f / 440) + 69

    // Simplified approximation:
    // At block 4, fnum 653 ~= 440 Hz (A4 = MIDI 69)
    // Each block doubles/halves the frequency (±12 semitones)

    // Calculate approximate Hz
    // Using simplified formula: F ≈ fnum * 0.0535 * 2^block (for standard clock)
    double freq = (double)fnum * 0.053516666 * (1 << block);

    if (freq < 8.0) {
        return 0; // Below MIDI range
    }

    // Convert to MIDI note
    double midi = 12.0 * log2(freq / 440.0) + 69.0;

    if (midi < 0) {
        return 0;
    }
    if (midi > 127) {
        return 127;
    }

    return (u8)(midi + 0.5);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void ym2612_process(VgmChipHandler* handler, u8 port, u8 reg, u8 value, u32 sample_time,
                           VgmEventBuffer* events) {
    Ym2612State* state = (Ym2612State*)handler->state;

    // Key on/off register (both ports write to same register)
    if (port == 0 && reg == 0x28) {
        u8 ch = value & 0x07;
        // Channel mapping: 0-2 are channels 0-2, 4-6 are channels 3-5
        if (ch >= 4) {
            ch = ch - 4 + 3;
        }
        if (ch >= 6) {
            return; // Invalid channel
        }

        u8 operators = (value >> 4) & 0x0F;
        Ym2612Channel* channel = &state->channels[ch];
        bool was_active = channel->active;
        bool is_active = operators != 0;

        channel->key_state = operators;
        channel->active = is_active;

        if (is_active && !was_active) {
            // Note on
            u8 note = vgm_ym2612_freq_to_note(channel->fnum, channel->block);
            VgmNoteEvent event = {
                .sample_time = sample_time,
                .chip_id = VGM_CHIP_YM2612,
                .chip_instance = handler->chip_instance,
                .channel = ch,
                .type = VGM_NOTE_ON,
                .frequency_raw = channel->fnum,
                .note = note,
                .octave = channel->block,
                .velocity = 127 - channel->total_level[0], // Use carrier TL as velocity approximation
            };
            vgm_event_buffer_push(events, &event);
            channel->last_note = note; // Track for NOTE_CHANGE filtering
        } else if (!is_active && was_active) {
            // Note off
            VgmNoteEvent event = {
                .sample_time = sample_time,
                .chip_id = VGM_CHIP_YM2612,
                .chip_instance = handler->chip_instance,
                .channel = ch,
                .type = VGM_NOTE_OFF,
                .frequency_raw = channel->fnum,
                .note = vgm_ym2612_freq_to_note(channel->fnum, channel->block),
                .octave = channel->block,
                .velocity = 0,
            };
            vgm_event_buffer_push(events, &event);
        }
        return;
    }

    // Frequency registers
    // Port 0: channels 0-2, Port 1: channels 3-5
    u8 ch_base = (port == 0) ? 0 : 3;

    if (reg >= 0xA4 && reg <= 0xA6) {
        // Frequency high byte + block (write this first)
        u8 ch = ch_base + (reg - 0xA4);
        state->pending_fnum_high[ch] = value;
        return;
    }

    if (reg >= 0xA0 && reg <= 0xA2) {
        // Frequency low byte (completes the frequency write)
        u8 ch = ch_base + (reg - 0xA0);
        Ym2612Channel* channel = &state->channels[ch];

        u8 high = state->pending_fnum_high[ch];
        u16 old_fnum = channel->fnum;
        u8 old_block = channel->block;

        channel->fnum = ((high & 0x07) << 8) | value;
        channel->block = (high >> 3) & 0x07;

        // If channel is active and frequency changed, check if MIDI note changed
        if (channel->active && (channel->fnum != old_fnum || channel->block != old_block)) {
            u8 new_note = vgm_ym2612_freq_to_note(channel->fnum, channel->block);
            // Only emit NOTE_CHANGE if the MIDI note actually changed (filters vibrato)
            if (new_note != channel->last_note) {
                VgmNoteEvent event = {
                    .sample_time = sample_time,
                    .chip_id = VGM_CHIP_YM2612,
                    .chip_instance = handler->chip_instance,
                    .channel = ch,
                    .type = VGM_NOTE_CHANGE,
                    .frequency_raw = channel->fnum,
                    .note = new_note,
                    .octave = channel->block,
                    .velocity = 127 - channel->total_level[0],
                };
                vgm_event_buffer_push(events, &event);
                channel->last_note = new_note;
            }
        }
        return;
    }

    // Total Level registers (0x40-0x4F for operators)
    if (reg >= 0x40 && reg <= 0x4F) {
        u8 ch = ch_base + (reg & 0x03);
        if (ch < 6 && (reg & 0x03) < 3) {
            u8 op = (reg - 0x40) / 4;
            if (op < 4) {
                Ym2612Channel* channel = &state->channels[ch];
                u8 old_tl = channel->total_level[op];
                u8 new_tl = value & 0x7F;
                channel->total_level[op] = new_tl;

                // Determine if this operator is a carrier based on algorithm
                // YM2612 carrier operators by algorithm:
                // Alg 0-3: only op 3 is carrier
                // Alg 4: op 1 and 3 are carriers
                // Alg 5-6: op 1, 2, 3 are carriers
                // Alg 7: all ops (0-3) are carriers
                bool is_carrier = false;
                switch (channel->algorithm) {
                    case 0:
                    case 1:
                    case 2:
                    case 3:
                        is_carrier = (op == 3);
                        break;
                    case 4:
                        is_carrier = (op == 1 || op == 3);
                        break;
                    case 5:
                    case 6:
                        is_carrier = (op == 1 || op == 2 || op == 3);
                        break;
                    case 7:
                        is_carrier = true; // All operators are carriers
                        break;
                }

#if VGM_CHIPS_DEBUG
                // Debug: only show TL changes when channel IS active (real volume changes during notes)
                if (ch == 0 && sample_time < 132300 && old_tl != new_tl && channel->active) {
                    double time_sec = sample_time / 44100.0;
                    printf("YM2612 TL ACTIVE: ch=%d op=%d alg=%d carrier=%d TL=%d->%d time=%.3fs vel=%d\n", ch, op,
                           channel->algorithm, is_carrier, old_tl, new_tl, time_sec, 127 - new_tl);
                }
#endif

                // Emit volume change event if channel is active and carrier TL changed
                if (channel->active && is_carrier && old_tl != new_tl) {
                    VgmNoteEvent event = {
                        .sample_time = sample_time,
                        .chip_id = VGM_CHIP_YM2612,
                        .chip_instance = handler->chip_instance,
                        .channel = ch,
                        .type = VGM_VOLUME_CHANGE,
                        .frequency_raw = channel->fnum,
                        .note = vgm_ym2612_freq_to_note(channel->fnum, channel->block),
                        .octave = channel->block,
                        .velocity = 127 - new_tl, // TL is inverted: 0 = loud, 127 = silent
                    };
                    vgm_event_buffer_push(events, &event);
                }
            }
        }
        return;
    }

    // Algorithm register (0xB0-0xB2)
    if (reg >= 0xB0 && reg <= 0xB2) {
        u8 ch = ch_base + (reg - 0xB0);
        if (ch < 6) {
            u8 old_alg = state->channels[ch].algorithm;
            state->channels[ch].algorithm = value & 0x07;
#if VGM_CHIPS_DEBUG
            if (ch == 0 && sample_time < 132300 && old_alg != (value & 0x07)) {
                double time_sec = sample_time / 44100.0;
                printf("YM2612 ALG: ch=%d alg=%d->%d time=%.3fs\n", ch, old_alg, value & 0x07, time_sec);
            }
#endif
        }
        return;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void ym2612_reset(VgmChipHandler* handler) {
    Ym2612State* state = (Ym2612State*)handler->state;
    memset(state->channels, 0, sizeof(state->channels));
    memset(state->pending_fnum_high, 0, sizeof(state->pending_fnum_high));
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

VgmChipHandler* vgm_chip_ym2612_create(VgmAllocator* alloc, u32 clock, u8 chip_instance) {
    VgmChipHandler* handler = (VgmChipHandler*)vgm_alloc(alloc, sizeof(VgmChipHandler));
    Ym2612State* state = (Ym2612State*)vgm_alloc(alloc, sizeof(Ym2612State));

    state->clock = clock;

    handler->state = state;
    handler->chip_id = VGM_CHIP_YM2612;
    handler->chip_instance = chip_instance;
    handler->channel_count = 6;
    handler->process = ym2612_process;
    handler->reset = ym2612_reset;

    return handler;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// SN76489 Handler
//
// The SN76489 is a 4-channel PSG chip (3 tone + 1 noise).
// It uses a simple register interface with latch/data bytes:
// - Latch byte: 1CCTDDDD (C=channel 0-3, T=type 0=tone/1=volume, D=data)
// - Data byte:  0-DDDDDD (continues previous latch)
//
// Volume is used as the gate - volume 0xF = silent (note off).
// Frequency is a 10-bit divider (higher = lower pitch).
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct Sn76489Channel {
    u16 tone;    // 10-bit tone divider
    u8 volume;   // 4-bit volume (0=loud, 15=silent)
    bool active; // Is channel currently producing sound
} Sn76489Channel;

typedef struct Sn76489State {
    Sn76489Channel channels[4];
    u32 clock;
    u8 latched_channel;
    u8 latched_type; // 0=tone, 1=volume
} Sn76489State;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

u8 vgm_sn76489_freq_to_note(u16 divider, u32 clock) {
    if (divider == 0) {
        return 0;
    }

    // SN76489 frequency formula:
    // F = clock / (32 * divider)
    double freq = (double)clock / (32.0 * divider);

    if (freq < 20.0) {
        return 0; // Below audible range
    }
    if (freq > 20000.0) {
        return 127; // Above audible range
    }

    // Convert to MIDI note
    double midi = 12.0 * log2(freq / 440.0) + 69.0;

    if (midi < 0) {
        return 0;
    }
    if (midi > 127) {
        return 127;
    }

    return (u8)(midi + 0.5);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void sn76489_process(VgmChipHandler* handler, u8 port, u8 reg, u8 value, u32 sample_time,
                            VgmEventBuffer* events) {
    (void)port;
    (void)reg;

    Sn76489State* state = (Sn76489State*)handler->state;

#if VGM_CHIPS_DEBUG
    // Debug: show ALL PSG writes for first 3 seconds
    if (sample_time < 132300) {
        double time_sec = sample_time / 44100.0;
        u32 row = sample_time / 735;
        bool is_latch = (value & 0x80) != 0;
        if (is_latch) {
            u8 channel = (value >> 5) & 0x03;
            u8 type = (value >> 4) & 0x01;
            u8 data = value & 0x0F;
            printf("SN76489 WRITE: time=%.3fs sample=%u row=%u LATCH ch=%d %s data=0x%X\n", time_sec, sample_time, row,
                   channel, type ? "VOL" : "TONE", data);
        } else {
            printf("SN76489 WRITE: time=%.3fs sample=%u row=%u DATA val=0x%02X (ch=%d %s)\n", time_sec, sample_time,
                   row, value, state->latched_channel, state->latched_type ? "VOL" : "TONE");
        }
    }
#endif

    if (value & 0x80) {
        // Latch byte
        u8 channel = (value >> 5) & 0x03;
        u8 type = (value >> 4) & 0x01;
        u8 data = value & 0x0F;

        state->latched_channel = channel;
        state->latched_type = type;

        Sn76489Channel* ch = &state->channels[channel];

        if (type == 1) {
            // Volume write
            u8 old_volume = ch->volume;
            ch->volume = data;

            bool was_active = old_volume < 0x0F;
            bool is_active = data < 0x0F;

            if (is_active && !was_active) {
                // Note on
                u8 note = (channel < 3) ? vgm_sn76489_freq_to_note(ch->tone, state->clock) : 0;
                VgmNoteEvent event = {
                    .sample_time = sample_time,
                    .chip_id = VGM_CHIP_SN76489,
                    .chip_instance = handler->chip_instance,
                    .channel = channel,
                    .type = VGM_NOTE_ON,
                    .frequency_raw = ch->tone,
                    .note = note,
                    .octave = 0,
                    .velocity = (u8)((0x0F - data) * 8), // Scale 0-15 to ~0-120
                };
                ch->active = true;
#if VGM_CHIPS_DEBUG
                if (sample_time < 132300) {
                    double time_sec = sample_time / 44100.0;
                    u32 row = sample_time / 735;
                    printf("  -> SN76489 NOTE_ON ch%d: time=%.3fs row=%u note=%d tone=%u vol=%d\n", channel, time_sec,
                           row, note, ch->tone, data);
                }
#endif
                vgm_event_buffer_push(events, &event);
            } else if (!is_active && was_active) {
                // Note off
                u8 note = (channel < 3) ? vgm_sn76489_freq_to_note(ch->tone, state->clock) : 0;
                VgmNoteEvent event = {
                    .sample_time = sample_time,
                    .chip_id = VGM_CHIP_SN76489,
                    .chip_instance = handler->chip_instance,
                    .channel = channel,
                    .type = VGM_NOTE_OFF,
                    .frequency_raw = ch->tone,
                    .note = note,
                    .octave = 0,
                    .velocity = 0,
                };
                ch->active = false;
#if VGM_CHIPS_DEBUG
                if (sample_time < 132300) {
                    double time_sec = sample_time / 44100.0;
                    u32 row = sample_time / 735;
                    printf("  -> SN76489 NOTE_OFF ch%d: time=%.3fs row=%u note=%d\n", channel, time_sec, row, note);
                }
#endif
                vgm_event_buffer_push(events, &event);
            } else if (is_active && was_active && old_volume != data) {
                // Volume change while active
                u8 note = (channel < 3) ? vgm_sn76489_freq_to_note(ch->tone, state->clock) : 0;
                VgmNoteEvent event = {
                    .sample_time = sample_time,
                    .chip_id = VGM_CHIP_SN76489,
                    .chip_instance = handler->chip_instance,
                    .channel = channel,
                    .type = VGM_VOLUME_CHANGE,
                    .frequency_raw = ch->tone,
                    .note = note,
                    .octave = 0,
                    .velocity = (u8)((0x0F - data) * 8), // Scale 0-15 to ~0-120
                };
                vgm_event_buffer_push(events, &event);
            }
        } else {
            // Tone low 4 bits
            u16 old_tone = ch->tone;
            ch->tone = (ch->tone & 0x3F0) | data;

            // If active and tone changed, emit note change
            if (ch->active && ch->tone != old_tone && channel < 3) {
                u8 note = vgm_sn76489_freq_to_note(ch->tone, state->clock);
                VgmNoteEvent event = {
                    .sample_time = sample_time,
                    .chip_id = VGM_CHIP_SN76489,
                    .chip_instance = handler->chip_instance,
                    .channel = channel,
                    .type = VGM_NOTE_CHANGE,
                    .frequency_raw = ch->tone,
                    .note = note,
                    .octave = 0,
                    .velocity = (u8)((0x0F - ch->volume) * 8),
                };
#if VGM_CHIPS_DEBUG
                if (sample_time < 132300) {
                    double time_sec = sample_time / 44100.0;
                    u32 row = sample_time / 735;
                    printf("  -> SN76489 NOTE_CHG ch%d: time=%.3fs row=%u note=%d tone=%u\n", channel, time_sec, row,
                           note, ch->tone);
                }
#endif
                vgm_event_buffer_push(events, &event);
            }
        }
    } else {
        // Data byte (continues previous latch)
        u8 channel = state->latched_channel;
        Sn76489Channel* ch = &state->channels[channel];

        if (state->latched_type == 0 && channel < 3) {
            // Tone high 6 bits
            u16 old_tone = ch->tone;
            ch->tone = (ch->tone & 0x00F) | ((value & 0x3F) << 4);

            // If active and tone changed, emit note change
            if (ch->active && ch->tone != old_tone) {
                u8 note = vgm_sn76489_freq_to_note(ch->tone, state->clock);
                VgmNoteEvent event = {
                    .sample_time = sample_time,
                    .chip_id = VGM_CHIP_SN76489,
                    .chip_instance = handler->chip_instance,
                    .channel = channel,
                    .type = VGM_NOTE_CHANGE,
                    .frequency_raw = ch->tone,
                    .note = note,
                    .octave = 0,
                    .velocity = (u8)((0x0F - ch->volume) * 8),
                };
#if VGM_CHIPS_DEBUG
                if (sample_time < 132300) {
                    double time_sec = sample_time / 44100.0;
                    u32 row = sample_time / 735;
                    printf("  -> SN76489 NOTE_CHG ch%d: time=%.3fs row=%u note=%d tone=%u (high byte)\n", channel,
                           time_sec, row, note, ch->tone);
                }
#endif
                vgm_event_buffer_push(events, &event);
            }
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void sn76489_reset(VgmChipHandler* handler) {
    Sn76489State* state = (Sn76489State*)handler->state;
    memset(state->channels, 0, sizeof(state->channels));
    // Initialize volumes to silent
    for (int i = 0; i < 4; i++) {
        state->channels[i].volume = 0x0F;
    }
    state->latched_channel = 0;
    state->latched_type = 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

VgmChipHandler* vgm_chip_sn76489_create(VgmAllocator* alloc, u32 clock, u8 chip_instance) {
    VgmChipHandler* handler = (VgmChipHandler*)vgm_alloc(alloc, sizeof(VgmChipHandler));
    Sn76489State* state = (Sn76489State*)vgm_alloc(alloc, sizeof(Sn76489State));

    state->clock = clock;
    // Initialize volumes to silent
    for (int i = 0; i < 4; i++) {
        state->channels[i].volume = 0x0F;
    }

    handler->state = state;
    handler->chip_id = VGM_CHIP_SN76489;
    handler->chip_instance = chip_instance;
    handler->channel_count = 4;
    handler->process = sn76489_process;
    handler->reset = sn76489_reset;

    return handler;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// AY8910/YM2149 frequency conversion
//
// The AY8910 uses a 12-bit tone period register.
// Frequency = clock / (16 * period)
// For period = 0, treat as period = 1 to avoid division by zero.

u8 vgm_ay8910_freq_to_note(u16 period, u32 clock) {
    if (period == 0) {
        return 0; // Invalid/silent
    }

    // Calculate actual frequency: freq = clock / (16 * period)
    float freq = (float)clock / (16.0f * (float)period);

    // Convert frequency to MIDI note number
    // MIDI note 69 = A4 = 440 Hz
    // note = 69 + 12 * log2(freq / 440)
    if (freq <= 0) {
        return 0;
    }

    float note_float = 69.0f + 12.0f * log2f(freq / 440.0f);

    // Clamp to valid MIDI range
    if (note_float < 0) {
        return 0;
    }
    if (note_float > 127) {
        return 127;
    }

    return (u8)(note_float + 0.5f); // Round to nearest
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// AY8910/YM2149 state tracking
//
// The AY8910 has 3 tone channels (A, B, C) plus noise.
// Register layout:
//   0-1: Channel A period (fine/coarse)
//   2-3: Channel B period (fine/coarse)
//   4-5: Channel C period (fine/coarse)
//   6:   Noise period
//   7:   Mixer control (enable bits: tone A/B/C, noise A/B/C)
//   8-10: Volume for channels A/B/C (bits 0-3 = volume, bit 4 = envelope mode)
//   11-13: Envelope period and shape

typedef struct Ay8910ChannelState {
    u16 period;         // 12-bit tone period
    u8 volume;          // 4-bit volume (0 = max, 15 = off, or envelope if bit 4 set)
    bool tone_enabled;  // Tone generator enabled
    bool noise_enabled; // Noise generator enabled
    bool use_envelope;  // Using envelope for volume
    u8 last_note;       // Last MIDI note for this channel
    u8 last_velocity;   // Last velocity for volume change detection
} Ay8910ChannelState;

typedef struct Ay8910State {
    Ay8910ChannelState channels[3];
    u8 mixer;  // Mixer control register
    u32 clock; // Chip clock rate
} Ay8910State;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Simple approach: emit events for every state change that affects sound
// - NOTE_ON when channel becomes audible (has volume + enabled + valid period)
// - NOTE_OFF when channel becomes silent
// - NOTE_CHANGE when note changes while audible

static inline bool ay8910_is_audible(const Ay8910ChannelState* ch) {
    // AY8910 volume: 0 = silent, 15 = loudest (when not using envelope)
    // Channel is audible when: (has volume OR uses envelope) AND (tone OR noise enabled) AND valid period
    return (ch->volume > 0 || ch->use_envelope) && (ch->tone_enabled || ch->noise_enabled) && ch->period > 0;
}

static void ay8910_emit_event(VgmChipHandler* handler, Ay8910State* state, int ch, bool was_audible, u32 sample_time,
                              VgmEventBuffer* events) {
    Ay8910ChannelState* channel = &state->channels[ch];
    bool is_audible = ay8910_is_audible(channel);

    if (!is_audible && !was_audible) {
        return; // Nothing to emit
    }

    u8 note = (channel->period > 0) ? vgm_ay8910_freq_to_note(channel->period, state->clock) : 0;
    u8 velocity = channel->use_envelope ? 100 : (u8)((15 - channel->volume) * 127 / 15);

#if VGM_CHIPS_DEBUG
    // Debug output for first 3 seconds (132300 samples at 44100Hz)
    if (sample_time < 132300) {
        double time_sec = sample_time / 44100.0;
        u32 row = sample_time / 735;
        printf("AY8910 ch%d: time=%.3fs sample=%u row=%u period=%u vol=%u env=%d tone=%d noise=%d was=%d is=%d\n", ch,
               time_sec, sample_time, row, channel->period, channel->volume, channel->use_envelope,
               channel->tone_enabled, channel->noise_enabled, was_audible, is_audible);
    }
#endif

    if (is_audible && !was_audible && note > 0) {
        // Became audible - NOTE_ON
#if VGM_CHIPS_DEBUG
        if (sample_time < 132300) {
            double time_sec = sample_time / 44100.0;
            u32 row = sample_time / 735;
            printf("  -> NOTE_ON ch%d: time=%.3fs row=%u note=%d period=%u\n", ch, time_sec, row, note,
                   channel->period);
        }
#endif
        VgmNoteEvent event = {
            .sample_time = sample_time,
            .chip_id = handler->chip_id,
            .chip_instance = handler->chip_instance,
            .channel = (u8)ch,
            .type = VGM_NOTE_ON,
            .frequency_raw = channel->period,
            .note = note,
            .octave = note / 12,
            .velocity = velocity,
        };
        vgm_event_buffer_push(events, &event);
        channel->last_note = note;
        channel->last_velocity = velocity;
    } else if (!is_audible && was_audible) {
        // Became silent - NOTE_OFF
#if VGM_CHIPS_DEBUG
        if (sample_time < 132300) {
            double time_sec = sample_time / 44100.0;
            u32 row = sample_time / 735;
            printf("  -> NOTE_OFF ch%d: time=%.3fs row=%u note=%d\n", ch, time_sec, row, channel->last_note);
        }
#endif
        VgmNoteEvent event = {
            .sample_time = sample_time,
            .chip_id = handler->chip_id,
            .chip_instance = handler->chip_instance,
            .channel = (u8)ch,
            .type = VGM_NOTE_OFF,
            .frequency_raw = channel->period,
            .note = channel->last_note,
            .octave = channel->last_note / 12,
            .velocity = 0,
        };
        vgm_event_buffer_push(events, &event);
    } else if (is_audible && was_audible && note > 0 && note != channel->last_note) {
        // Note changed while audible - NOTE_CHANGE
#if VGM_CHIPS_DEBUG
        if (sample_time < 132300) {
            double time_sec = sample_time / 44100.0;
            u32 row = sample_time / 735;
            printf("  -> NOTE_CHG ch%d: time=%.3fs row=%u note=%d->%d period=%u\n", ch, time_sec, row,
                   channel->last_note, note, channel->period);
        }
#endif
        VgmNoteEvent event = {
            .sample_time = sample_time,
            .chip_id = handler->chip_id,
            .chip_instance = handler->chip_instance,
            .channel = (u8)ch,
            .type = VGM_NOTE_CHANGE,
            .frequency_raw = channel->period,
            .note = note,
            .octave = note / 12,
            .velocity = velocity,
        };
        vgm_event_buffer_push(events, &event);
        channel->last_note = note;
        channel->last_velocity = velocity;
    } else if (is_audible && was_audible && velocity != channel->last_velocity) {
        // Volume changed while audible - VOLUME_CHANGE
        VgmNoteEvent event = {
            .sample_time = sample_time,
            .chip_id = handler->chip_id,
            .chip_instance = handler->chip_instance,
            .channel = (u8)ch,
            .type = VGM_VOLUME_CHANGE,
            .frequency_raw = channel->period,
            .note = note,
            .octave = note / 12,
            .velocity = velocity,
        };
        vgm_event_buffer_push(events, &event);
        channel->last_velocity = velocity;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void ay8910_process(VgmChipHandler* handler, u8 port, u8 reg, u8 value, u32 sample_time,
                           VgmEventBuffer* events) {
    (void)port; // AY8910 doesn't use port addressing
    Ay8910State* state = (Ay8910State*)handler->state;

#if VGM_CHIPS_DEBUG
    // Debug: show ALL register writes for first 3 seconds
    if (sample_time < 132300) {
        double time_sec = sample_time / 44100.0;
        u32 row = sample_time / 735;
        printf("AY8910 WRITE: time=%.3fs sample=%u row=%u reg=%d val=0x%02X\n", time_sec, sample_time, row, reg, value);
    }
#endif

    switch (reg) {
        // Channel A period (low byte)
        case 0: {
            bool was_audible = ay8910_is_audible(&state->channels[0]);
            state->channels[0].period = (state->channels[0].period & 0x0F00) | value;
            ay8910_emit_event(handler, state, 0, was_audible, sample_time, events);
            break;
        }
        // Channel A period (high byte)
        case 1: {
            bool was_audible = ay8910_is_audible(&state->channels[0]);
            state->channels[0].period = (state->channels[0].period & 0x00FF) | ((value & 0x0F) << 8);
            ay8910_emit_event(handler, state, 0, was_audible, sample_time, events);
            break;
        }
        // Channel B period (low byte)
        case 2: {
            bool was_audible = ay8910_is_audible(&state->channels[1]);
            state->channels[1].period = (state->channels[1].period & 0x0F00) | value;
            ay8910_emit_event(handler, state, 1, was_audible, sample_time, events);
            break;
        }
        // Channel B period (high byte)
        case 3: {
            bool was_audible = ay8910_is_audible(&state->channels[1]);
            state->channels[1].period = (state->channels[1].period & 0x00FF) | ((value & 0x0F) << 8);
            ay8910_emit_event(handler, state, 1, was_audible, sample_time, events);
            break;
        }
        // Channel C period (low byte)
        case 4: {
            bool was_audible = ay8910_is_audible(&state->channels[2]);
            state->channels[2].period = (state->channels[2].period & 0x0F00) | value;
            ay8910_emit_event(handler, state, 2, was_audible, sample_time, events);
            break;
        }
        // Channel C period (high byte)
        case 5: {
            bool was_audible = ay8910_is_audible(&state->channels[2]);
            state->channels[2].period = (state->channels[2].period & 0x00FF) | ((value & 0x0F) << 8);
            ay8910_emit_event(handler, state, 2, was_audible, sample_time, events);
            break;
        }

        // Mixer control (bits 0-2 = tone disable, bits 3-5 = noise disable, active low)
        case 7: {
            bool was_audible[3];
            for (int ch = 0; ch < 3; ch++) {
                was_audible[ch] = ay8910_is_audible(&state->channels[ch]);
            }
            state->mixer = value;
            state->channels[0].tone_enabled = !(value & 0x01);
            state->channels[1].tone_enabled = !(value & 0x02);
            state->channels[2].tone_enabled = !(value & 0x04);
            state->channels[0].noise_enabled = !(value & 0x08);
            state->channels[1].noise_enabled = !(value & 0x10);
            state->channels[2].noise_enabled = !(value & 0x20);
            for (int ch = 0; ch < 3; ch++) {
                ay8910_emit_event(handler, state, ch, was_audible[ch], sample_time, events);
            }
            break;
        }

        // Channel volumes (8=A, 9=B, 10=C)
        case 8:
        case 9:
        case 10: {
            int ch = reg - 8;
            bool was_audible = ay8910_is_audible(&state->channels[ch]);
            state->channels[ch].volume = value & 0x0F;
            state->channels[ch].use_envelope = (value & 0x10) != 0;
            ay8910_emit_event(handler, state, ch, was_audible, sample_time, events);
            break;
        }

        default:
            // Ignore other registers (noise period, envelope)
            break;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void ay8910_reset(VgmChipHandler* handler) {
    Ay8910State* state = (Ay8910State*)handler->state;
    memset(state->channels, 0, sizeof(state->channels));
    // Default: tone enabled, noise disabled, volume silent (15 = off)
    state->mixer = 0x38;
    for (int i = 0; i < 3; i++) {
        state->channels[i].tone_enabled = true;
        state->channels[i].volume = 15; // Silent by default
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

VgmChipHandler* vgm_chip_ay8910_create(VgmAllocator* alloc, u32 clock, u8 chip_instance) {
    VgmChipHandler* handler = (VgmChipHandler*)vgm_alloc(alloc, sizeof(VgmChipHandler));
    Ay8910State* state = (Ay8910State*)vgm_alloc(alloc, sizeof(Ay8910State));

    state->clock = clock;
    // Default: tone enabled, noise disabled, volume silent (15 = off)
    state->mixer = 0x38;
    for (int i = 0; i < 3; i++) {
        state->channels[i].tone_enabled = true;
        state->channels[i].volume = 15; // Silent by default
    }

    handler->state = state;
    handler->chip_id = VGM_CHIP_AY8910;
    handler->chip_instance = chip_instance;
    handler->channel_count = 3; // 3 tone channels (noise not tracked as separate channel)
    handler->process = ay8910_process;
    handler->reset = ay8910_reset;

    return handler;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
