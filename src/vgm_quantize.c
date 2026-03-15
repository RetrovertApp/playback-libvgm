///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// VGM Quantization - Per-channel adaptive implementation
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "vgm_quantize.h"
#include "vgm_alloc.h"
#include <string.h>

#define VGM_QUANTIZE_DEBUG 0

#if VGM_QUANTIZE_DEBUG
#include <stdio.h>
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Status strings

const char* vgm_quantize_status_string(VgmQuantizeStatus status) {
    switch (status) {
        case VGM_QUANTIZE_OK:
            return "Success";
        case VGM_QUANTIZE_ERROR_NULL_TIMELINE:
            return "Null timeline pointer";
        case VGM_QUANTIZE_ERROR_INVALID_CONFIG:
            return "Invalid configuration";
        case VGM_QUANTIZE_ERROR_ALLOCATION_FAILED:
            return "Memory allocation failed";
        default:
            return "Unknown error";
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: count events per channel

static void count_events_per_channel(const VgmTimeline* timeline, u32* counts) {
    memset(counts, 0, timeline->channel_count * sizeof(u32));

    for (u32 i = 0; i < timeline->event_count; i++) {
        const VgmNoteEvent* event = &timeline->events[i];
        i32 ch_idx = vgm_timeline_get_channel_index(timeline, event->chip_id, event->chip_instance, event->channel);
        if (ch_idx >= 0 && (u32)ch_idx < timeline->channel_count) {
            counts[ch_idx]++;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: find 10th percentile gap between consecutive events for a channel
// Using percentile avoids outliers (single very close events) from making scroll too fast

static u32 find_percentile_gap_for_channel(const VgmTimeline* timeline, u32 channel_idx, u32 event_count) {
    if (event_count < 2) {
        return UINT32_MAX;
    }

    // Collect gaps into temporary array (max 1000 gaps to avoid huge allocations)
    u32 gaps[1000];
    u32 gap_count = 0;
    u32 last_sample_time = 0;
    bool first_event = true;

    for (u32 i = 0; i < timeline->event_count && gap_count < 1000; i++) {
        const VgmNoteEvent* event = &timeline->events[i];
        i32 ch_idx = vgm_timeline_get_channel_index(timeline, event->chip_id, event->chip_instance, event->channel);
        if (ch_idx != (i32)channel_idx) {
            continue;
        }

        if (!first_event) {
            u32 gap = event->sample_time - last_sample_time;
            if (gap > 0) {
                gaps[gap_count++] = gap;
            }
        }

        last_sample_time = event->sample_time;
        first_event = false;
    }

    if (gap_count == 0) {
        return UINT32_MAX;
    }

    // Simple selection sort to find 10th percentile (we only need partial sort)
    u32 target_idx = gap_count / 10; // 10th percentile
    if (target_idx == 0) {
        target_idx = 0; // At least get the minimum if very few gaps
    }

    // Partial sort: find the target_idx smallest element
    for (u32 i = 0; i <= target_idx && i < gap_count; i++) {
        u32 min_idx = i;
        for (u32 j = i + 1; j < gap_count; j++) {
            if (gaps[j] < gaps[min_idx]) {
                min_idx = j;
            }
        }
        // Swap
        u32 tmp = gaps[i];
        gaps[i] = gaps[min_idx];
        gaps[min_idx] = tmp;
    }

    return gaps[target_idx];
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: calculate optimal samples_per_row for a channel based on event density

static u32 calculate_channel_spr(const VgmTimeline* timeline, u32 channel_idx, u32 event_count,
                                 VgmQuantizeConfig config) {
    if (event_count == 0) {
        return config.max_samples_per_row;
    }

    // Find 10th percentile gap (handles outliers better than minimum)
    u32 gap = find_percentile_gap_for_channel(timeline, channel_idx, event_count);

    // If no valid gap found (only one event), use max
    if (gap == UINT32_MAX) {
        return config.max_samples_per_row;
    }

    // Set spr based on gap so dense sections are readable
    // Multiply by 2 to give some breathing room
    u32 spr = gap * 2;

    // Clamp to min/max
    if (spr < config.min_samples_per_row) {
        spr = config.min_samples_per_row;
    }
    if (spr > config.max_samples_per_row) {
        spr = config.max_samples_per_row;
    }

    return spr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: quantize single channel

static bool quantize_channel(VgmAllocator* alloc, const VgmTimeline* timeline, u32 channel_idx,
                             VgmChannelPattern* out_channel, VgmQuantizeConfig config) {
    // Count events for this channel
    u32 event_count = 0;
    for (u32 i = 0; i < timeline->event_count; i++) {
        const VgmNoteEvent* event = &timeline->events[i];
        i32 ch_idx = vgm_timeline_get_channel_index(timeline, event->chip_id, event->chip_instance, event->channel);
        if (ch_idx == (i32)channel_idx) {
            event_count++;
        }
    }

    out_channel->event_count = event_count;

    // Calculate samples_per_row for this channel based on densest section
    u32 spr = calculate_channel_spr(timeline, channel_idx, event_count, config);
    out_channel->samples_per_row = spr;

    // Calculate row count
    u32 total_rows = (timeline->total_samples + spr - 1) / spr;
    if (total_rows == 0) {
        total_rows = 1;
    }

    // If no events, create empty channel
    if (event_count == 0) {
        out_channel->rows = nullptr;
        out_channel->row_count = 0;
        return true;
    }

    // Allocate rows (one per quantized position that has events)
    // First pass: count unique rows
    u32 unique_rows = 0;
    u32 last_row = UINT32_MAX;
    for (u32 i = 0; i < timeline->event_count; i++) {
        const VgmNoteEvent* event = &timeline->events[i];
        i32 ch_idx = vgm_timeline_get_channel_index(timeline, event->chip_id, event->chip_instance, event->channel);
        if (ch_idx != (i32)channel_idx) {
            continue;
        }
        u32 row = event->sample_time / spr;
        if (row != last_row) {
            unique_rows++;
            last_row = row;
        }
    }

    if (unique_rows == 0) {
        out_channel->rows = nullptr;
        out_channel->row_count = 0;
        return true;
    }

    // Allocate rows
    out_channel->rows = (VgmChannelRow*)vgm_alloc_array(alloc, unique_rows * sizeof(VgmChannelRow));
    if (out_channel->rows == nullptr) {
        return false;
    }

    // Initialize all cells to empty
    memset(out_channel->rows, 0, unique_rows * sizeof(VgmChannelRow));

    // Second pass: fill in rows
    u32 current_row_idx = 0;
    last_row = UINT32_MAX;

    // Priority for note types: NOTE_ON > NOTE_CHANGE > NOTE_OFF
    static const int type_priority[] = { 2, 0, 1 }; // [NOTE_ON, NOTE_OFF, NOTE_CHANGE]

    for (u32 i = 0; i < timeline->event_count; i++) {
        const VgmNoteEvent* event = &timeline->events[i];
        i32 ch_idx = vgm_timeline_get_channel_index(timeline, event->chip_id, event->chip_instance, event->channel);
        if (ch_idx != (i32)channel_idx) {
            continue;
        }

        u32 row_num = event->sample_time / spr;

        // Start new row if needed
        if (row_num != last_row) {
            if (current_row_idx >= unique_rows) {
                break; // Safety check
            }
            out_channel->rows[current_row_idx].sample_time = row_num * spr;
            current_row_idx++;
            last_row = row_num;
        }

        VgmPatternCell* cell = &out_channel->rows[current_row_idx - 1].cell;

        // Handle volume change events as effects (can coexist with notes)
        if (event->type == VGM_VOLUME_CHANGE) {
            cell->has_effect = true;
            cell->effect_type = VGM_EFFECT_VOLUME;
            cell->effect_value = event->velocity;
            continue;
        }

        // Priority filtering for note events
        int new_priority = (event->type <= VGM_NOTE_CHANGE) ? type_priority[event->type] : -1;

        if (cell->has_note) {
            int existing_priority = (cell->type <= VGM_NOTE_CHANGE) ? type_priority[cell->type] : -1;
            if (new_priority <= existing_priority) {
                continue; // Existing event has equal or higher priority
            }
        }

        cell->has_note = true;
        cell->type = event->type;
        cell->note = event->note;
        cell->velocity = event->velocity;
    }

    out_channel->row_count = current_row_idx;

#if VGM_QUANTIZE_DEBUG
    printf("Channel %u: %u events, spr=%u, %u rows\n", channel_idx, event_count, spr, out_channel->row_count);
#endif

    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Main quantization function

VgmQuantizeResult vgm_quantize(VgmAllocator* alloc, const VgmTimeline* timeline, VgmQuantizeConfig config) {
    VgmQuantizeResult result = { 0 };

    // Validate inputs
    if (timeline == nullptr) {
        result.status = VGM_QUANTIZE_ERROR_NULL_TIMELINE;
        return result;
    }

    // Apply defaults if not set
    if (config.min_samples_per_row == 0) {
        config.min_samples_per_row = VGM_QUANTIZE_DEFAULT_MIN_SPR;
    }
    if (config.max_samples_per_row == 0) {
        config.max_samples_per_row = VGM_QUANTIZE_DEFAULT_MAX_SPR;
    }
    if (config.target_rows_visible == 0) {
        config.target_rows_visible = VGM_QUANTIZE_DEFAULT_VISIBLE;
    }

    // Allocate pattern
    VgmPattern* pattern = (VgmPattern*)vgm_alloc(alloc, sizeof(VgmPattern));
    if (pattern == nullptr) {
        result.status = VGM_QUANTIZE_ERROR_ALLOCATION_FAILED;
        return result;
    }

    pattern->total_samples = timeline->total_samples;
    pattern->channel_count = timeline->channel_count;

    // Copy channel info
    if (timeline->channel_count > 0) {
        pattern->channel_info = (VgmChannelInfo*)vgm_alloc_array(alloc, timeline->channel_count * sizeof(VgmChannelInfo));
        if (pattern->channel_info == nullptr) {
            result.status = VGM_QUANTIZE_ERROR_ALLOCATION_FAILED;
            return result;
        }
        memcpy(pattern->channel_info, timeline->channels, timeline->channel_count * sizeof(VgmChannelInfo));

        // Allocate per-channel patterns
        pattern->channels = (VgmChannelPattern*)vgm_alloc_array(alloc, timeline->channel_count * sizeof(VgmChannelPattern));
        if (pattern->channels == nullptr) {
            result.status = VGM_QUANTIZE_ERROR_ALLOCATION_FAILED;
            return result;
        }
        memset(pattern->channels, 0, timeline->channel_count * sizeof(VgmChannelPattern));

        // Quantize each channel independently
        for (u32 ch = 0; ch < timeline->channel_count; ch++) {
            if (!quantize_channel(alloc, timeline, ch, &pattern->channels[ch], config)) {
                result.status = VGM_QUANTIZE_ERROR_ALLOCATION_FAILED;
                return result;
            }
        }
    }

#if VGM_QUANTIZE_DEBUG
    printf("VGM Quantize: %u channels, %u total samples\n", pattern->channel_count, pattern->total_samples);
    for (u32 ch = 0; ch < pattern->channel_count; ch++) {
        printf("  Channel %u (%s): %u rows, spr=%u\n", ch, pattern->channel_info[ch].name,
               pattern->channels[ch].row_count, pattern->channels[ch].samples_per_row);
    }
#endif

    result.pattern = pattern;
    result.status = VGM_QUANTIZE_OK;
    return result;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Query functions

u32 vgm_channel_find_row(const VgmChannelPattern* channel, u32 sample_position) {
    if (channel == nullptr || channel->row_count == 0) {
        return 0;
    }

    // If before first row, return 0
    if (sample_position < channel->rows[0].sample_time) {
        return 0;
    }

    // If after last row, return last row
    if (sample_position >= channel->rows[channel->row_count - 1].sample_time) {
        return channel->row_count - 1;
    }

    // Binary search for the largest row where sample_time <= sample_position
    u32 left = 0;
    u32 right = channel->row_count;

    while (left < right) {
        u32 mid = left + (right - left + 1) / 2; // Round up
        if (mid < channel->row_count && channel->rows[mid].sample_time <= sample_position) {
            left = mid;
        } else {
            right = mid - 1;
        }
    }

    return left;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const VgmPatternCell* vgm_pattern_get_channel_cell(const VgmPattern* pattern, u32 channel_index, u32 row_index) {
    if (pattern == nullptr) {
        return nullptr;
    }

    if (channel_index >= pattern->channel_count) {
        return nullptr;
    }

    const VgmChannelPattern* channel = &pattern->channels[channel_index];
    if (row_index >= channel->row_count) {
        return nullptr;
    }

    return &channel->rows[row_index].cell;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
