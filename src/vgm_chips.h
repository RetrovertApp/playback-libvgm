///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// VGM Chip Handlers - Extract note events from chip register writes
//
// Each chip handler tracks internal state and emits note events when
// musically significant register writes occur (key-on, key-off, frequency changes).
//
// Supported chips:
// - YM2612 (Genesis/Mega Drive FM)
// - SN76489 (Genesis/Mega Drive PSG, Master System, Game Gear)
// - AY8910/YM2149 (MSX, ZX Spectrum, Atari ST, Amstrad CPC)
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "vgm_parser.h"

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Note event types

typedef enum VgmNoteEventType {
    VGM_NOTE_ON = 0,   // Note started
    VGM_NOTE_OFF,      // Note ended
    VGM_NOTE_CHANGE,   // Frequency changed while note active
    VGM_VOLUME_CHANGE, // Volume changed while note active
} VgmNoteEventType;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Note event structure

typedef struct VgmNoteEvent {
    u32 sample_time;       // Absolute sample position when event occurs
    VgmChipId chip_id;     // Which chip type
    u8 chip_instance;      // 0 = first chip, 1 = second (dual chip setups)
    u8 channel;            // Channel within chip (0-5 for YM2612, 0-3 for PSG)
    VgmNoteEventType type; // NOTE_ON, NOTE_OFF, NOTE_CHANGE
    u16 frequency_raw;     // Chip-specific raw frequency value
    u8 note;               // MIDI note number (0-127)
    u8 octave;             // Octave number
    u8 velocity;           // Approximated velocity (0-127)
} VgmNoteEvent;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Event buffer for collecting emitted events

typedef struct VgmEventBuffer {
    VgmNoteEvent* events; // Array of events
    u32 count;            // Number of events in buffer
    u32 capacity;         // Maximum capacity
} VgmEventBuffer;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Chip handler interface

typedef struct VgmChipHandler VgmChipHandler;

// Process a register write to this chip
typedef void (*VgmChipProcessFn)(VgmChipHandler* handler, u8 port, u8 reg, u8 value, u32 sample_time,
                                 VgmEventBuffer* events);

// Reset chip state to initial
typedef void (*VgmChipResetFn)(VgmChipHandler* handler);

struct VgmChipHandler {
    void* state;              // Chip-specific state
    VgmChipId chip_id;        // Chip type
    u8 chip_instance;         // 0 or 1 for dual chip
    u8 channel_count;         // Number of channels this chip has
    VgmChipProcessFn process; // Process register write
    VgmChipResetFn reset;     // Reset state
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Event buffer management

// Initialize event buffer with given capacity (allocates from arena)
VgmEventBuffer vgm_event_buffer_create(VgmAllocator* alloc, u32 capacity);

// Add event to buffer (returns false if full)
bool vgm_event_buffer_push(VgmEventBuffer* buffer, const VgmNoteEvent* event);

// Clear buffer (reset count to 0)
void vgm_event_buffer_clear(VgmEventBuffer* buffer);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Chip handler creation

// Create YM2612 handler (6-channel FM synthesis)
// clock: Chip clock rate in Hz (typically 7670453 for NTSC Genesis)
VgmChipHandler* vgm_chip_ym2612_create(VgmAllocator* alloc, u32 clock, u8 chip_instance);

// Create SN76489 handler (4-channel PSG: 3 tone + 1 noise)
// clock: Chip clock rate in Hz (typically 3579545)
VgmChipHandler* vgm_chip_sn76489_create(VgmAllocator* alloc, u32 clock, u8 chip_instance);

// Create AY8910/YM2149 handler (3-channel PSG + noise)
// clock: Chip clock rate in Hz (typically 1789773 or 2000000)
VgmChipHandler* vgm_chip_ay8910_create(VgmAllocator* alloc, u32 clock, u8 chip_instance);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Frequency to note conversion utilities

// Convert YM2612 F-number and block to MIDI note
// fnum: 11-bit frequency number (0-2047)
// block: 3-bit octave/block (0-7)
// Returns MIDI note number (0-127) or 0 if invalid
u8 vgm_ym2612_freq_to_note(u16 fnum, u8 block);

// Convert SN76489 tone divider to MIDI note
// divider: 10-bit tone divider (0-1023)
// clock: Chip clock rate in Hz
// Returns MIDI note number (0-127) or 0 if invalid
u8 vgm_sn76489_freq_to_note(u16 divider, u32 clock);

// Convert AY8910 tone period to MIDI note
// period: 12-bit tone period (0-4095)
// clock: Chip clock rate in Hz
// Returns MIDI note number (0-127) or 0 if invalid
u8 vgm_ay8910_freq_to_note(u16 period, u32 clock);

// Get note name string (e.g., "C-4", "F#5")
// Returns pointer to static string
const char* vgm_note_name(u8 midi_note);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Channel naming

// Get channel name for display (e.g., "FM 1", "PSG 0", "Noise")
// chip_id: Chip type
// channel: Channel number within chip
// Returns pointer to static string
const char* vgm_channel_name(VgmChipId chip_id, u8 channel);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
