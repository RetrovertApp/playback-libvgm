///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// VGM Quantization - Map sample times to display rows
//
// Converts a VGM timeline (sample-accurate events) into a quantized pattern
// suitable for tracker-style display. Each channel is quantized independently
// based on its event density, allowing per-channel scroll rates.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "vgm_timeline.h"

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Effect types for pattern display

typedef enum VgmEffectType {
    VGM_EFFECT_NONE = 0,
    VGM_EFFECT_VOLUME,      // Volume change (value = 0-127)
    VGM_EFFECT_PITCH_SLIDE, // Pitch slide (for future use)
} VgmEffectType;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Pattern cell - one channel's data for one row

typedef struct VgmPatternCell {
    bool has_note;             // True if this cell has note data
    bool has_effect;           // True if this cell has effect data
    VgmNoteEventType type;     // NOTE_ON, NOTE_OFF, NOTE_CHANGE, VOLUME_CHANGE
    u8 note;                   // MIDI note number (0-127)
    u8 velocity;               // Velocity (0-127)
    VgmEffectType effect_type; // Effect type (VOLUME, etc.)
    u8 effect_value;           // Effect value
} VgmPatternCell;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Per-channel row - single cell with its sample time

typedef struct VgmChannelRow {
    u32 sample_time;     // Sample position of this row
    VgmPatternCell cell; // Cell data for this channel
} VgmChannelRow;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Per-channel pattern - independent row list and timing

typedef struct VgmChannelPattern {
    VgmChannelRow* rows; // Array of rows for this channel
    u32 row_count;       // Number of rows
    u32 samples_per_row; // Quantization rate for this channel
    u32 event_count;     // Original event count (for density calculation)
} VgmChannelPattern;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Quantized pattern - per-channel patterns with independent timing

typedef struct VgmPattern {
    // Per-channel patterns (each channel scrolls independently)
    VgmChannelPattern* channels;
    u32 channel_count;

    // Channel info (copied from timeline)
    VgmChannelInfo* channel_info;

    // Global timing info
    u32 total_samples; // Total duration
} VgmPattern;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Configuration

typedef struct VgmQuantizeConfig {
    u32 min_samples_per_row; // Minimum samples per row (avoid too fast scrolling)
    u32 max_samples_per_row; // Maximum samples per row (avoid too slow scrolling)
    u32 target_rows_visible; // Target number of rows visible on screen (for density calc)
} VgmQuantizeConfig;

// Default configuration
#define VGM_QUANTIZE_DEFAULT_MIN_SPR 200  // ~220 rows/sec max at 44100Hz
#define VGM_QUANTIZE_DEFAULT_MAX_SPR 8820 // ~5 rows/sec min at 44100Hz
#define VGM_QUANTIZE_DEFAULT_VISIBLE 19   // Typical visible rows

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Result type

typedef enum VgmQuantizeStatus {
    VGM_QUANTIZE_OK = 0,
    VGM_QUANTIZE_ERROR_NULL_TIMELINE,
    VGM_QUANTIZE_ERROR_INVALID_CONFIG,
    VGM_QUANTIZE_ERROR_ALLOCATION_FAILED,
} VgmQuantizeStatus;

typedef struct VgmQuantizeResult {
    VgmPattern* pattern;
    VgmQuantizeStatus status;
} VgmQuantizeResult;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// API

// Create a quantized pattern from a timeline with per-channel adaptive timing
// Each channel gets its own samples_per_row based on event density
VgmQuantizeResult vgm_quantize(VgmAllocator* alloc, const VgmTimeline* timeline, VgmQuantizeConfig config);

// Get human-readable error message
const char* vgm_quantize_status_string(VgmQuantizeStatus status);

// Convert sample position to row index for a specific channel
static inline u32 vgm_channel_sample_to_row(u32 sample, u32 samples_per_row) {
    if (samples_per_row == 0)
        return 0;
    return sample / samples_per_row;
}

// Convert row index to sample position for a specific channel
static inline u32 vgm_channel_row_to_sample(u32 row, u32 samples_per_row) {
    return row * samples_per_row;
}

// Find row index for a given sample position in a channel using binary search
// Returns the row at or before the sample position
u32 vgm_channel_find_row(const VgmChannelPattern* channel, u32 sample_position);

// Get the cell for a specific channel and row
// Returns nullptr if out of bounds
const VgmPatternCell* vgm_pattern_get_channel_cell(const VgmPattern* pattern, u32 channel_index, u32 row_index);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
