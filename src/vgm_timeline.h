///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// VGM Timeline - Single-pass extraction of note events from VGM files
//
// Iterates through VGM commands, routes register writes to chip handlers,
// and collects all emitted note events into a sorted timeline.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "vgm_chips.h"
#include "vgm_parser.h"

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Constants

#define VGM_MAX_CHANNELS 32 // Maximum channels across all chips

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Channel metadata

typedef struct VgmChannelInfo {
    VgmChipId chip_id; // Which chip type
    u8 chip_instance;  // 0 or 1 for dual chip
    u8 chip_channel;   // Channel within chip (0-5 for YM2612, 0-3 for PSG)
    const char* name;  // Display name ("FM 1", "PSG 2", "Noise")
} VgmChannelInfo;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Timeline structure

typedef struct VgmTimeline {
    // Note events (sorted by sample_time)
    VgmNoteEvent* events;
    u32 event_count;
    u32 event_capacity;

    // Timing info
    u32 total_samples;     // Total duration in samples
    u32 loop_start_sample; // Loop point (0 if no loop)

    // Channel info
    VgmChannelInfo channels[VGM_MAX_CHANNELS];
    u32 channel_count;
} VgmTimeline;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Result type

typedef enum VgmTimelineStatus {
    VGM_TIMELINE_OK = 0,
    VGM_TIMELINE_ERROR_NULL_FILE,
    VGM_TIMELINE_ERROR_ALLOCATION_FAILED,
    VGM_TIMELINE_ERROR_NO_CHIPS,
} VgmTimelineStatus;

typedef struct VgmTimelineResult {
    VgmTimeline* timeline;
    VgmTimelineStatus status;
} VgmTimelineResult;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// API

// Extract all note events from a parsed VGM file
// Creates chip handlers for present chips, iterates commands, collects events
VgmTimelineResult vgm_timeline_create(VgmAllocator* alloc, const VgmFile* file);

// Get human-readable error message
const char* vgm_timeline_status_string(VgmTimelineStatus status);

// Get channel index for a chip/channel combination
// Returns -1 if not found
i32 vgm_timeline_get_channel_index(const VgmTimeline* timeline, VgmChipId chip_id, u8 chip_instance, u8 chip_channel);

// Get events in a sample range (inclusive)
// Returns pointer to first event in range, sets out_count
// Returns nullptr if no events in range
const VgmNoteEvent* vgm_timeline_events_in_range(const VgmTimeline* timeline, u32 start_sample, u32 end_sample,
                                                 u32* out_count);

// Get all events for a specific channel
// Caller provides output buffer, returns count written
u32 vgm_timeline_get_channel_events(const VgmTimeline* timeline, u32 channel_index, VgmNoteEvent* out_events,
                                    u32 max_events);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
