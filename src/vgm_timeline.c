///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// VGM Timeline - Implementation
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "vgm_timeline.h"
#include "vgm_alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VGM_TIMELINE_DEBUG 0

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Status strings

const char* vgm_timeline_status_string(VgmTimelineStatus status) {
    switch (status) {
        case VGM_TIMELINE_OK:
            return "Success";
        case VGM_TIMELINE_ERROR_NULL_FILE:
            return "Null VGM file pointer";
        case VGM_TIMELINE_ERROR_ALLOCATION_FAILED:
            return "Memory allocation failed";
        case VGM_TIMELINE_ERROR_NO_CHIPS:
            return "No supported chips in VGM file";
        default:
            return "Unknown error";
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper to add channel info

static u32 add_chip_channels(VgmTimeline* timeline, VgmChipId chip_id, u8 chip_instance, u8 num_channels) {
    u32 base_index = timeline->channel_count;

    for (u8 i = 0; i < num_channels && timeline->channel_count < VGM_MAX_CHANNELS; i++) {
        VgmChannelInfo* info = &timeline->channels[timeline->channel_count];
        info->chip_id = chip_id;
        info->chip_instance = chip_instance;
        info->chip_channel = i;
        info->name = vgm_channel_name(chip_id, i);
        timeline->channel_count++;
    }

    return base_index;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper to grow event buffer if needed

static bool ensure_event_capacity(VgmAllocator* alloc, VgmTimeline* timeline, u32 additional) {
    u32 needed = timeline->event_count + additional;
    if (needed <= timeline->event_capacity) {
        return true;
    }

    // Grow by 2x or to needed, whichever is larger
    u32 new_capacity = timeline->event_capacity * 2;
    if (new_capacity < needed) {
        new_capacity = needed;
    }
    if (new_capacity < 1024) {
        new_capacity = 1024;
    }

    VgmNoteEvent* new_events = (VgmNoteEvent*)vgm_realloc(alloc, timeline->events,
                                                            new_capacity * sizeof(VgmNoteEvent));
    if (new_events == nullptr) {
        return false;
    }

    // Zero newly allocated portion
    if (timeline->event_count < new_capacity) {
        memset(&new_events[timeline->event_count], 0,
               (new_capacity - timeline->event_count) * sizeof(VgmNoteEvent));
    }

    timeline->events = new_events;
    timeline->event_capacity = new_capacity;
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper to copy events from buffer to timeline

static bool copy_events_to_timeline(VgmAllocator* alloc, VgmTimeline* timeline, const VgmEventBuffer* buffer) {
    if (buffer->count == 0) {
        return true;
    }

    if (!ensure_event_capacity(alloc, timeline, buffer->count)) {
        return false;
    }

    memcpy(&timeline->events[timeline->event_count], buffer->events, buffer->count * sizeof(VgmNoteEvent));
    timeline->event_count += buffer->count;
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Compare function for sorting events by sample time

static int compare_events(const void* a, const void* b) {
    const VgmNoteEvent* ea = (const VgmNoteEvent*)a;
    const VgmNoteEvent* eb = (const VgmNoteEvent*)b;

    if (ea->sample_time < eb->sample_time)
        return -1;
    if (ea->sample_time > eb->sample_time)
        return 1;

    // Secondary sort by chip and channel for stability
    if (ea->chip_id < eb->chip_id)
        return -1;
    if (ea->chip_id > eb->chip_id)
        return 1;
    if (ea->channel < eb->channel)
        return -1;
    if (ea->channel > eb->channel)
        return 1;

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Main timeline creation

VgmTimelineResult vgm_timeline_create(VgmAllocator* alloc, const VgmFile* file) {
    VgmTimelineResult result = { 0 };

    if (file == nullptr) {
        result.status = VGM_TIMELINE_ERROR_NULL_FILE;
        return result;
    }

    // Allocate timeline
    VgmTimeline* timeline = (VgmTimeline*)vgm_alloc(alloc, sizeof(VgmTimeline));
    if (timeline == nullptr) {
        result.status = VGM_TIMELINE_ERROR_ALLOCATION_FAILED;
        return result;
    }

    timeline->total_samples = file->total_samples;
    timeline->loop_start_sample = file->loop_offset;

    // Create chip handlers for present chips
    VgmChipHandler* ym2612_handler = nullptr;
    VgmChipHandler* sn76489_handler = nullptr;
    VgmChipHandler* ay8910_handler = nullptr;

    // Check for YM2612
    if (file->chips[VGM_CHIP_YM2612].present) {
        ym2612_handler = vgm_chip_ym2612_create(alloc, file->chips[VGM_CHIP_YM2612].clock, 0);
        if (ym2612_handler != nullptr) {
            add_chip_channels(timeline, VGM_CHIP_YM2612, 0, ym2612_handler->channel_count);
        }
    }

    // Check for SN76489
    if (file->chips[VGM_CHIP_SN76489].present) {
        sn76489_handler = vgm_chip_sn76489_create(alloc, file->chips[VGM_CHIP_SN76489].clock, 0);
        if (sn76489_handler != nullptr) {
            add_chip_channels(timeline, VGM_CHIP_SN76489, 0, sn76489_handler->channel_count);
        }
    }

    // Check for AY8910/YM2149
    if (file->chips[VGM_CHIP_AY8910].present) {
        ay8910_handler = vgm_chip_ay8910_create(alloc, file->chips[VGM_CHIP_AY8910].clock, 0);
        if (ay8910_handler != nullptr) {
            add_chip_channels(timeline, VGM_CHIP_AY8910, 0, ay8910_handler->channel_count);
        }
    }

    // Check we have at least one chip
    if (ym2612_handler == nullptr && sn76489_handler == nullptr && ay8910_handler == nullptr) {
        result.status = VGM_TIMELINE_ERROR_NO_CHIPS;
        return result;
    }

#if VGM_TIMELINE_DEBUG
    printf("VGM Timeline: Detected chips:\n");
    if (ym2612_handler)
        printf("  - YM2612 (clock=%u, channels=%u)\n", file->chips[VGM_CHIP_YM2612].clock,
               ym2612_handler->channel_count);
    if (sn76489_handler)
        printf("  - SN76489 (clock=%u, channels=%u)\n", file->chips[VGM_CHIP_SN76489].clock,
               sn76489_handler->channel_count);
    if (ay8910_handler)
        printf("  - AY8910 (clock=%u, channels=%u)\n", file->chips[VGM_CHIP_AY8910].clock,
               ay8910_handler->channel_count);
    printf("  Total channels: %u\n", timeline->channel_count);
#endif

    // Create event buffer for collecting events during iteration
    VgmEventBuffer event_buffer = vgm_event_buffer_create(alloc, 256);
    if (event_buffer.events == nullptr) {
        result.status = VGM_TIMELINE_ERROR_ALLOCATION_FAILED;
        return result;
    }

    // Iterate through VGM commands
    VgmIterator iter = vgm_iterator_create(file);
    VgmCommand cmd;

#if VGM_TIMELINE_DEBUG
    u32 psg_write_count = 0;
    u32 ym2612_write_count = 0;
    u32 ay8910_write_count = 0;
#endif

    while (vgm_iterator_next(&iter, &cmd)) {
        // Clear event buffer before processing
        vgm_event_buffer_clear(&event_buffer);

        switch (cmd.type) {
            case VGM_CMD_WRITE_PSG:
#if VGM_TIMELINE_DEBUG
                psg_write_count++;
#endif
                if (sn76489_handler != nullptr) {
                    sn76489_handler->process(sn76489_handler, cmd.port, cmd.reg, cmd.value, cmd.sample_time,
                                             &event_buffer);
                }
                break;

            case VGM_CMD_WRITE_YM2612_P0:
#if VGM_TIMELINE_DEBUG
                ym2612_write_count++;
#endif
                if (ym2612_handler != nullptr) {
                    ym2612_handler->process(ym2612_handler, 0, cmd.reg, cmd.value, cmd.sample_time, &event_buffer);
                }
                break;

            case VGM_CMD_WRITE_YM2612_P1:
#if VGM_TIMELINE_DEBUG
                ym2612_write_count++;
#endif
                if (ym2612_handler != nullptr) {
                    ym2612_handler->process(ym2612_handler, 1, cmd.reg, cmd.value, cmd.sample_time, &event_buffer);
                }
                break;

            case VGM_CMD_WRITE_AY8910:
#if VGM_TIMELINE_DEBUG
                ay8910_write_count++;
#endif
                if (ay8910_handler != nullptr) {
                    ay8910_handler->process(ay8910_handler, cmd.port, cmd.reg, cmd.value, cmd.sample_time,
                                            &event_buffer);
                }
                break;

            case VGM_CMD_END:
                // End of data
                break;

            default:
                // Ignore other commands (waits, data blocks, etc.)
                break;
        }

        // Copy any emitted events to timeline
        if (event_buffer.count > 0) {
#if VGM_TIMELINE_DEBUG
            // Debug: print events as they're detected (first 3 seconds only = 132300 samples at 44100Hz)
            for (u32 i = 0; i < event_buffer.count; i++) {
                const VgmNoteEvent* ev = &event_buffer.events[i];
                if (ev->sample_time < 132300) {
                    const char* type_names[] = { "ON", "OFF", "CHG" };
                    const char* type_str = (ev->type <= VGM_NOTE_CHANGE) ? type_names[ev->type] : "???";
                    double time_sec = ev->sample_time / 44100.0;
                    u32 row = ev->sample_time / 735; // 60Hz row mapping
                    printf("EXTRACTED: time=%.3fs sample=%u row=%u ch=%d %s note=%d period=%u\n", time_sec,
                           ev->sample_time, row, ev->channel, type_str, ev->note, ev->frequency_raw);
                }
            }
#endif
            if (!copy_events_to_timeline(alloc, timeline, &event_buffer)) {
                result.status = VGM_TIMELINE_ERROR_ALLOCATION_FAILED;
                return result;
            }
        }
    }

    // Sort events by sample time (should already be mostly sorted)
    if (timeline->event_count > 1) {
        qsort(timeline->events, timeline->event_count, sizeof(VgmNoteEvent), compare_events);
    }

#if VGM_TIMELINE_DEBUG
    printf("VGM Timeline: Command counts:\n");
    printf("  PSG writes: %u\n", psg_write_count);
    printf("  YM2612 writes: %u\n", ym2612_write_count);
    printf("  AY8910 writes: %u\n", ay8910_write_count);
    printf("  Total events extracted: %u\n", timeline->event_count);
#endif

    result.timeline = timeline;
    result.status = VGM_TIMELINE_OK;
    return result;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Query functions

i32 vgm_timeline_get_channel_index(const VgmTimeline* timeline, VgmChipId chip_id, u8 chip_instance, u8 chip_channel) {
    if (timeline == nullptr) {
        return -1;
    }

    for (u32 i = 0; i < timeline->channel_count; i++) {
        const VgmChannelInfo* info = &timeline->channels[i];
        if (info->chip_id == chip_id && info->chip_instance == chip_instance && info->chip_channel == chip_channel) {
            return (i32)i;
        }
    }

    return -1;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const VgmNoteEvent* vgm_timeline_events_in_range(const VgmTimeline* timeline, u32 start_sample, u32 end_sample,
                                                 u32* out_count) {
    if (timeline == nullptr || timeline->event_count == 0 || out_count == nullptr) {
        if (out_count)
            *out_count = 0;
        return nullptr;
    }

    // Binary search for first event >= start_sample
    u32 left = 0;
    u32 right = timeline->event_count;

    while (left < right) {
        u32 mid = left + (right - left) / 2;
        if (timeline->events[mid].sample_time < start_sample) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    u32 first = left;
    if (first >= timeline->event_count || timeline->events[first].sample_time > end_sample) {
        *out_count = 0;
        return nullptr;
    }

    // Find last event <= end_sample
    u32 last = first;
    while (last < timeline->event_count && timeline->events[last].sample_time <= end_sample) {
        last++;
    }

    *out_count = last - first;
    return &timeline->events[first];
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

u32 vgm_timeline_get_channel_events(const VgmTimeline* timeline, u32 channel_index, VgmNoteEvent* out_events,
                                    u32 max_events) {
    if (timeline == nullptr || out_events == nullptr || max_events == 0) {
        return 0;
    }

    if (channel_index >= timeline->channel_count) {
        return 0;
    }

    const VgmChannelInfo* channel = &timeline->channels[channel_index];
    u32 count = 0;

    for (u32 i = 0; i < timeline->event_count && count < max_events; i++) {
        const VgmNoteEvent* event = &timeline->events[i];
        if (event->chip_id == channel->chip_id && event->chip_instance == channel->chip_instance
            && event->channel == channel->chip_channel) {
            out_events[count++] = *event;
        }
    }

    return count;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
