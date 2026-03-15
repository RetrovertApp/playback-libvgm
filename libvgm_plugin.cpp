///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// libvgm Playback Plugin
//
// Implements RVPlaybackPlugin interface for video game music formats using libvgm:
// VGM, VGZ (compressed VGM), S98, GYM, and DRO.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include <retrovert/io.h>
#include <retrovert/log.h>
#include <retrovert/metadata.h>
#include <retrovert/playback.h>
#include <retrovert/service.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// libvgm headers
#include "emu/EmuCores.h"
#include "player/droplayer.hpp"
#include "player/gymplayer.hpp"
#include "player/playera.hpp"
#include "player/s98player.hpp"
#include "player/vgmplayer.hpp"
#include "utils/DataLoader.h"
#include "utils/MemoryLoader.h"

// VGM pattern extraction (pure C)
#ifdef HAS_VGM_PATTERN
extern "C" {
#include "src/vgm_alloc.h"
#include "src/vgm_parser.h"
#include "src/vgm_quantize.h"
#include "src/vgm_timeline.h"
}
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Debug flag - must be defined before use

#define VGM_DEBUG_WRITES 0

#if VGM_DEBUG_WRITES
static int s_debug_cell_count = 0;
static uint32_t s_last_logged_row = UINT32_MAX;
static double s_last_logged_time = -1.0;
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Module-local globals for API access

RV_PLUGIN_USE_IO_API();
RV_PLUGIN_USE_METADATA_API();
RV_PLUGIN_USE_LOG_API();

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Constants

static const uint32_t SAMPLE_RATE = 48000;
static const float INT32_TO_FLOAT = 1.0f / 2147483648.0f;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Plugin instance data

struct LibvgmData {
    PlayerA* player;
    DATA_LOADER* dload;
    uint8_t* file_data;
    uint64_t file_size;
    uint8_t vu_left;
    uint8_t vu_right;

#ifdef HAS_VGM_PATTERN
    // VGM pattern extraction
    VgmAllocator* pattern_alloc;
    VgmPattern* pattern;
    uint32_t current_sample; // Tracks current playback position in samples
#endif

    // Scope visualization state
    bool scope_enabled;
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: case-insensitive string comparison

static int strcasecmp_local(const char* a, const char* b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : *b;
        if (ca != cb) {
            return ca - cb;
        }
        a++;
        b++;
    }
    char ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;
    char cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : *b;
    return ca - cb;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: get file extension from filename

static const char* get_extension(const char* filename) {
    if (filename == nullptr) {
        return nullptr;
    }
    const char* dot = strrchr(filename, '.');
    if (dot == nullptr || dot == filename) {
        return nullptr;
    }
    return dot + 1;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// RVPlaybackPlugin implementation

static const char* libvgm_supported_extensions(void) {
    return "vgm,vgz,s98,gym,dro";
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVProbeResult libvgm_probe_can_play(uint8_t* data, uint64_t data_size, const char* filename,
                                           uint64_t total_size) {
    (void)total_size;

    if (data == nullptr || data_size < 8) {
        return RVProbeResult_Unsupported;
    }

    // VGM: "Vgm " at offset 0
    if (data[0] == 'V' && data[1] == 'g' && data[2] == 'm' && data[3] == ' ') {
        return RVProbeResult_Supported;
    }

    // VGZ: gzip magic (1F 8B)
    if (data[0] == 0x1F && data[1] == 0x8B) {
        // Check extension to confirm it's VGZ
        const char* ext = get_extension(filename);
        if (ext != nullptr) {
            if (strcasecmp_local(ext, "vgz") == 0) {
                return RVProbeResult_Supported;
            }
            // Also check for .vgm.gz
            const char* dot2 = strrchr(filename, '.');
            if (dot2 != nullptr && dot2 > filename) {
                // Look for second-to-last dot
                const char* p = dot2 - 1;
                while (p > filename && *p != '.') {
                    p--;
                }
                if (*p == '.' && strcasecmp_local(p, ".vgm.gz") == 0) {
                    return RVProbeResult_Supported;
                }
            }
        }
        return RVProbeResult_Unsure;
    }

    // S98: "S98" at offset 0
    if (data[0] == 'S' && data[1] == '9' && data[2] == '8') {
        return RVProbeResult_Supported;
    }

    // GYM with GYMX header
    if (data[0] == 'G' && data[1] == 'Y' && data[2] == 'M' && data[3] == 'X') {
        return RVProbeResult_Supported;
    }

    // DRO: "DBRAWOPL" at offset 0
    if (memcmp(data, "DBRAWOPL", 8) == 0) {
        return RVProbeResult_Supported;
    }

    // GYM without header - check extension
    const char* ext = get_extension(filename);
    if (ext != nullptr && strcasecmp_local(ext, "gym") == 0) {
        return RVProbeResult_Unsure;
    }

    return RVProbeResult_Unsupported;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void* libvgm_create(const RVService* service_api) {
    LibvgmData* data = static_cast<LibvgmData*>(malloc(sizeof(LibvgmData)));
    if (data == nullptr) {
        return nullptr;
    }
    memset(data, 0, sizeof(LibvgmData));

    // Create PlayerA instance
    data->player = new PlayerA();

    // Register all supported player engines
    data->player->RegisterPlayerEngine(new VGMPlayer());
    data->player->RegisterPlayerEngine(new S98Player());
    data->player->RegisterPlayerEngine(new GYMPlayer());
    data->player->RegisterPlayerEngine(new DROPlayer());

    // Configure output: 48kHz stereo 32-bit
    // NOTE: The smplBufferLen parameter must be > 0, otherwise libvgm crashes
    // when trying to access _smplBuf[0] on an empty vector.
    // 8192 samples is a reasonable buffer size.
    data->player->SetOutputSettings(SAMPLE_RATE, 2, 32, 8192);

    // Configure playback options
    data->player->SetLoopCount(2);                       // Play loops twice
    data->player->SetFadeSamples(SAMPLE_RATE * 3);       // 3 second fade out
    data->player->SetEndSilenceSamples(SAMPLE_RATE / 2); // 0.5 second silence at end

#ifdef HAS_VGM_PATTERN
    // Create allocator for pattern extraction
    data->pattern_alloc = vgm_alloc_create();
#endif

    return data;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int libvgm_destroy(void* user_data) {
    LibvgmData* data = static_cast<LibvgmData*>(user_data);
    if (data == nullptr) {
        return 0;
    }

    if (data->player != nullptr) {
        data->player->Stop();
        data->player->UnloadFile();

        // UnregisterAllPlayers handles deletion of registered player engines
        data->player->UnregisterAllPlayers();

        delete data->player;
        data->player = nullptr;
    }

    if (data->dload != nullptr) {
        DataLoader_Deinit(data->dload);
        data->dload = nullptr;
    }

    if (data->file_data != nullptr) {
        rv_io_free_url_to_memory(data->file_data);
        data->file_data = nullptr;
    }

#ifdef HAS_VGM_PATTERN
    // Clean up pattern extraction allocator
    vgm_alloc_destroy(data->pattern_alloc);
    data->pattern_alloc = nullptr;
#endif

    free(data);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int libvgm_open(void* user_data, const char* url, uint32_t subsong, const RVService* service_api) {
    (void)subsong;
    (void)service_api;

    LibvgmData* data = static_cast<LibvgmData*>(user_data);

    // Clean up any previously open file
    if (data->dload != nullptr) {
        data->player->Stop();
        data->player->UnloadFile();
        DataLoader_Deinit(data->dload);
        data->dload = nullptr;
    }

    if (data->file_data != nullptr) {
        rv_io_free_url_to_memory(data->file_data);
        data->file_data = nullptr;
    }

    data->vu_left = 0;
    data->vu_right = 0;

    // Load file into memory
    RVIoReadUrlResult read_res = rv_io_read_url_to_memory(url);
    if (read_res.data == nullptr) {
        rv_error("libvgm: Failed to load file: %s", url);
        return -1;
    }

    data->file_data = read_res.data;
    data->file_size = read_res.data_size;

    // Create memory loader for libvgm
    data->dload = MemoryLoader_Init(data->file_data, static_cast<UINT32>(data->file_size));
    if (data->dload == nullptr) {
        rv_error("libvgm: Failed to create data loader for: %s", url);
        rv_io_free_url_to_memory(data->file_data);
        data->file_data = nullptr;
        return -1;
    }

    // Load the data
    UINT8 load_result = DataLoader_Load(data->dload);
    if (load_result != 0) {
        rv_error("libvgm: DataLoader_Load failed for: %s (error %d)", url, load_result);
        DataLoader_Deinit(data->dload);
        data->dload = nullptr;
        rv_io_free_url_to_memory(data->file_data);
        data->file_data = nullptr;
        return -1;
    }

    // Load the file into the player
    UINT8 result = data->player->LoadFile(data->dload);
    if (result != 0) {
        rv_error("libvgm: Failed to load file into player: %s (error %d)", url, result);
        DataLoader_Deinit(data->dload);
        data->dload = nullptr;
        rv_io_free_url_to_memory(data->file_data);
        data->file_data = nullptr;
        return -1;
    }

#ifdef HAS_VGM_PATTERN
    // Extract VGM pattern data for visualization (VGM/VGZ files only)
    data->pattern = nullptr;
    data->current_sample = 0;
    if (data->pattern_alloc != nullptr) {
        // Reset allocator for new file
        vgm_alloc_rewind(data->pattern_alloc);

        // Get the decompressed VGM data from DataLoader
        // (handles both .vgm and .vgz files - libvgm decompresses gzip automatically)
        UINT8* vgm_data = DataLoader_GetData(data->dload);
        UINT32 vgm_size = DataLoader_GetSize(data->dload);

        // Check if this is a VGM file (magic "Vgm ")
        if (vgm_size >= 4 && vgm_data[0] == 'V' && vgm_data[1] == 'g' && vgm_data[2] == 'm' && vgm_data[3] == ' ') {
            // Parse VGM file
            VgmParseResult parse_result = vgm_parse(data->pattern_alloc, vgm_data, vgm_size);
            if (parse_result.status == VGM_PARSE_OK && parse_result.file != nullptr) {
                rv_debug("libvgm: Parsed VGM, total_samples=%u", parse_result.file->total_samples);

                // Extract note events into timeline
                VgmTimelineResult timeline_result = vgm_timeline_create(data->pattern_alloc, parse_result.file);
                if (timeline_result.status == VGM_TIMELINE_OK && timeline_result.timeline != nullptr) {
                    rv_debug("libvgm: Created timeline, events=%u, channels=%u", timeline_result.timeline->event_count,
                             timeline_result.timeline->channel_count);

                    // Per-channel quantization - each channel gets its own scroll rate
                    // based on event density
                    VgmQuantizeConfig quantize_config = {
                        .min_samples_per_row = VGM_QUANTIZE_DEFAULT_MIN_SPR,
                        .max_samples_per_row = VGM_QUANTIZE_DEFAULT_MAX_SPR,
                        .target_rows_visible = VGM_QUANTIZE_DEFAULT_VISIBLE,
                    };
                    VgmQuantizeResult quantize_result
                        = vgm_quantize(data->pattern_alloc, timeline_result.timeline, quantize_config);
                    if (quantize_result.status == VGM_QUANTIZE_OK && quantize_result.pattern != nullptr) {
                        data->pattern = quantize_result.pattern;
                        rv_info("libvgm: Created per-channel pattern with %u channels", data->pattern->channel_count);

                        // Debug: show per-channel info
                        for (uint32_t ch = 0; ch < data->pattern->channel_count; ch++) {
                            const VgmChannelPattern* channel = &data->pattern->channels[ch];
                            rv_info("  Channel %u (%s): %u rows, %u spr (%.1f Hz)", ch,
                                    data->pattern->channel_info[ch].name, channel->row_count, channel->samples_per_row,
                                    44100.0f / channel->samples_per_row);
                        }
                    } else {
                        rv_debug("libvgm: Quantization failed with status %d", quantize_result.status);
                    }
                } else {
                    rv_debug("libvgm: Timeline creation failed with status %d", timeline_result.status);
                }
            } else {
                rv_debug("libvgm: VGM parse failed with status %d", parse_result.status);
            }
        } else {
            rv_debug("libvgm: Not a VGM file (no magic header)");
        }
    }
#endif

    // Configure YM2612 to use Gens core for scope capture support
    // The Gens core is the only one with per-channel audio capture implemented
    // TEMPORARILY DISABLED - testing with MAME core
#if 0
    {
        PlayerBase* player = data->player->GetPlayer();
        if (player != nullptr) {
            PLR_DEV_OPTS devOpts;
            PlayerBase::InitDeviceOptions(devOpts);
            devOpts.emuCore[0] = FCC_GENS;
            // Set for both YM2612 instances (in case of dual chip)
            player->SetDeviceOptions(PLR_DEV_ID(0x02, 0), devOpts);  // YM2612 instance 0
            player->SetDeviceOptions(PLR_DEV_ID(0x02, 1), devOpts);  // YM2612 instance 1

        }
    }
#endif

    // Start playback
    result = data->player->Start();
    if (result != 0) {
        rv_error("libvgm: Failed to start playback: %s (error %d)", url, result);
        data->player->UnloadFile();
        DataLoader_Deinit(data->dload);
        data->dload = nullptr;
        rv_io_free_url_to_memory(data->file_data);
        data->file_data = nullptr;
        return -1;
    }

    rv_info("libvgm: Loaded %s (duration: %.2fs)", url, data->player->GetTotalTime(PLAYTIME_LOOP_INCL));

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void libvgm_close(void* user_data) {
    LibvgmData* data = static_cast<LibvgmData*>(user_data);

    if (data->player != nullptr) {
        data->player->Stop();
        data->player->UnloadFile();
    }

    if (data->dload != nullptr) {
        DataLoader_Deinit(data->dload);
        data->dload = nullptr;
    }

    if (data->file_data != nullptr) {
        rv_io_free_url_to_memory(data->file_data);
        data->file_data = nullptr;
    }

#ifdef HAS_VGM_PATTERN
    // Clear pattern data (arena memory will be reused on next open)
    data->pattern = nullptr;
    data->current_sample = 0;
#endif
    data->vu_left = 0;
    data->vu_right = 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVReadInfo libvgm_read_data(void* user_data, RVReadData dest) {
    LibvgmData* data = static_cast<LibvgmData*>(user_data);

    RVAudioFormat format = { RVAudioStreamFormat_F32, 2, SAMPLE_RATE };
    RVReadInfo info = { format, 0, RVReadStatus_Ok };

    if (data->player == nullptr || data->dload == nullptr) {
        info.status = RVReadStatus_Error;
        return info;
    }

    // Check if playback has finished
    UINT8 state = data->player->GetState();
    if (state & PLAYSTATE_FIN) {
        info.status = RVReadStatus_Finished;
        return info;
    }

    float* output = static_cast<float*>(dest.channels_output);
    uint32_t max_frames = dest.channels_output_max_bytes_size / (sizeof(float) * 2);

    // Allocate temporary buffer for WAVE_32BS output
    WAVE_32BS* temp_buffer = static_cast<WAVE_32BS*>(malloc(max_frames * sizeof(WAVE_32BS)));
    if (temp_buffer == nullptr) {
        info.status = RVReadStatus_Error;
        return info;
    }

    // Render audio
    UINT32 bytes_rendered = data->player->Render(max_frames * sizeof(WAVE_32BS), temp_buffer);
    UINT32 frames_rendered = bytes_rendered / sizeof(WAVE_32BS);

    // Convert WAVE_32BS (INT32 stereo) to float32 interleaved and track VU meters
    int32_t peak_left = 0;
    int32_t peak_right = 0;

    for (uint32_t i = 0; i < frames_rendered; i++) {
        int32_t left = temp_buffer[i].L;
        int32_t right = temp_buffer[i].R;

        output[i * 2 + 0] = static_cast<float>(left) * INT32_TO_FLOAT;
        output[i * 2 + 1] = static_cast<float>(right) * INT32_TO_FLOAT;

        // Track peak for VU meters (absolute value)
        int32_t abs_left = (left < 0) ? -left : left;
        int32_t abs_right = (right < 0) ? -right : right;
        if (abs_left > peak_left)
            peak_left = abs_left;
        if (abs_right > peak_right)
            peak_right = abs_right;
    }

    free(temp_buffer);

    // Convert peak to 0-255 VU value (logarithmic scale would be better, but linear is simpler)
    // Shift right by 23 bits to get top 8 bits of 31-bit absolute value
    data->vu_left = static_cast<uint8_t>((peak_left >> 23) & 0xFF);
    data->vu_right = static_cast<uint8_t>((peak_right >> 23) & 0xFF);

    info.frame_count = frames_rendered;

    // Check state again after rendering
    state = data->player->GetState();
    if (state & PLAYSTATE_FIN) {
        info.status = RVReadStatus_Finished;
    }

    return info;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int64_t libvgm_seek(void* user_data, int64_t ms) {
    LibvgmData* data = static_cast<LibvgmData*>(user_data);

    if (data->player == nullptr || ms < 0) {
        return -1;
    }

    // Convert milliseconds to samples
    uint32_t target_sample = static_cast<uint32_t>((ms * SAMPLE_RATE) / 1000);

    UINT8 result = data->player->Seek(PLAYPOS_SAMPLE, target_sample);
    if (result != 0) {
        rv_error("libvgm: Seek failed (error %d)", result);
        return -1;
    }

    // Return actual position in ms
    UINT32 cur_sample = data->player->GetCurPos(PLAYPOS_SAMPLE);
    return static_cast<int64_t>((static_cast<uint64_t>(cur_sample) * 1000) / SAMPLE_RATE);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int libvgm_metadata(const char* url, const RVService* service_api) {
    (void)service_api;

    // Load file
    RVIoReadUrlResult read_res = rv_io_read_url_to_memory(url);
    if (read_res.data == nullptr) {
        rv_error("libvgm: Failed to load file for metadata: %s", url);
        return -1;
    }

    // Create temporary player for metadata extraction
    PlayerA player;
    player.RegisterPlayerEngine(new VGMPlayer());
    player.RegisterPlayerEngine(new S98Player());
    player.RegisterPlayerEngine(new GYMPlayer());
    player.RegisterPlayerEngine(new DROPlayer());
    player.SetOutputSettings(SAMPLE_RATE, 2, 32, 8192);

    // Create data loader
    DATA_LOADER* dload = MemoryLoader_Init(read_res.data, static_cast<UINT32>(read_res.data_size));
    if (dload == nullptr) {
        // UnregisterAllPlayers handles deletion of registered engines
        player.UnregisterAllPlayers();
        rv_io_free_url_to_memory(read_res.data);
        return -1;
    }

    if (DataLoader_Load(dload) != 0) {
        DataLoader_Deinit(dload);
        player.UnregisterAllPlayers();
        rv_io_free_url_to_memory(read_res.data);
        return -1;
    }

    if (player.LoadFile(dload) != 0) {
        DataLoader_Deinit(dload);
        player.UnregisterAllPlayers();
        rv_io_free_url_to_memory(read_res.data);
        return -1;
    }

    // Create metadata entry
    RVMetadataId index = rv_metadata_create_url(url);

    // Get player base to access tags
    PlayerBase* base_player = player.GetPlayer();
    if (base_player != nullptr) {
        const char* const* tags = base_player->GetTags();
        if (tags != nullptr) {
            // Tags are in pairs: [type, value, type, value, ..., NULL]
            for (int i = 0; tags[i] != nullptr; i += 2) {
                const char* tag_type = tags[i];
                const char* tag_value = tags[i + 1];
                if (tag_value == nullptr || tag_value[0] == '\0') {
                    continue;
                }

                // Map libvgm tag types to RV metadata
                // VGM tags: TITLE, TITLE_JP, GAME, GAME_JP, SYSTEM, SYSTEM_JP, ARTIST, ARTIST_JP, DATE, CREATOR, NOTES
                if (strcmp(tag_type, "TITLE") == 0) {
                    rv_metadata_set_tag(index, RV_METADATA_TITLE_TAG, tag_value);
                } else if (strcmp(tag_type, "GAME") == 0) {
                    rv_metadata_set_tag(index, RV_METADATA_ALBUM_TAG, tag_value);
                } else if (strcmp(tag_type, "SYSTEM") == 0) {
                    rv_metadata_set_tag(index, RV_METADATA_SONGTYPE_TAG, tag_value);
                } else if (strcmp(tag_type, "ARTIST") == 0) {
                    rv_metadata_set_tag(index, RV_METADATA_ARTIST_TAG, tag_value);
                } else if (strcmp(tag_type, "DATE") == 0) {
                    rv_metadata_set_tag(index, RV_METADATA_DATE_TAG, tag_value);
                } else if (strcmp(tag_type, "NOTES") == 0) {
                    rv_metadata_set_tag(index, RV_METADATA_MESSAGE_TAG, tag_value);
                }
            }
        }

        // Get format name from player
        const char* player_name = base_player->GetPlayerName();
        if (player_name != nullptr) {
            rv_metadata_set_tag(index, RV_METADATA_SONGTYPE_TAG, player_name);
        }
    }

    // Get duration (including loops and fade)
    double total_time = player.GetTotalTime(PLAYTIME_LOOP_INCL | PLAYTIME_WITH_FADE);
    rv_metadata_set_tag_f64(index, RV_METADATA_LENGTH_TAG, total_time);

    // Clean up - UnregisterAllPlayers handles deletion of registered engines
    player.UnloadFile();
    DataLoader_Deinit(dload);
    player.UnregisterAllPlayers();
    rv_io_free_url_to_memory(read_res.data);

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void libvgm_event(void* user_data, uint8_t* event_data, uint64_t len) {
    LibvgmData* data = static_cast<LibvgmData*>(user_data);

#if VGM_DEBUG_WRITES
    static int s_event_call_count = 0;
    if (s_event_call_count < 5) {
        printf("libvgm_event CALLED: user_data=%p len=%lu\n", user_data, (unsigned long)len);
        fflush(stdout);
        s_event_call_count++;
    }
#endif

    if (len < 8 || data == nullptr) {
        return;
    }

    // Report VU meters based on recent peak amplitude
    event_data[0] = data->vu_left;
    event_data[1] = data->vu_right;
    event_data[2] = 0;
    event_data[3] = 0;

#ifdef HAS_VGM_PATTERN
    // Report current row based on playback position
    // Note: With per-channel scrolling, this legacy row value is only used for
    // compatibility. The real per-channel positions are handled via get_tracker_info().
    if (data->pattern != nullptr && data->player != nullptr && data->pattern->channel_count > 0) {
        // Get current sample position from player (at 44100Hz VGM rate)
        double current_time = data->player->GetCurTime(PLAYTIME_TIME_FILE);
        uint32_t current_sample = static_cast<uint32_t>(current_time * 44100.0);

        // Find first channel with events to get a representative row
        uint32_t current_row = 0;
        for (uint32_t ch = 0; ch < data->pattern->channel_count; ch++) {
            if (data->pattern->channels[ch].row_count > 0) {
                current_row = vgm_channel_find_row(&data->pattern->channels[ch], current_sample);
                break;
            }
        }

        // Store row as 16-bit little-endian in event_data[5:6]
        event_data[5] = static_cast<uint8_t>((current_row >> 8) & 0xFF); // High byte
        event_data[6] = static_cast<uint8_t>(current_row & 0xFF);        // Low byte
        event_data[7] = 0;                                               // Pattern number (VGM only has one pattern)
    } else
#endif
    {
        event_data[5] = 0;
        event_data[6] = 0;
        event_data[7] = 0;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Tracker visualization API

static int libvgm_get_tracker_info(void* user_data, RVTrackerInfo* info) {
    LibvgmData* data = static_cast<LibvgmData*>(user_data);

    if (data == nullptr || info == nullptr) {
        return -1;
    }

    memset(info, 0, sizeof(RVTrackerInfo));

    // Get song metadata from player tags
    if (data->player != nullptr) {
        PlayerBase* base_player = data->player->GetPlayer();
        if (base_player != nullptr) {
            const char* const* tags = base_player->GetTags();
            if (tags != nullptr) {
                const char* title = nullptr;
                const char* game = nullptr;
                const char* artist = nullptr;

                // Parse tags (pairs of type, value)
                for (int i = 0; tags[i] != nullptr; i += 2) {
                    const char* tag_type = tags[i];
                    const char* tag_value = tags[i + 1];
                    if (tag_value == nullptr || tag_value[0] == '\0') {
                        continue;
                    }
                    if (strcmp(tag_type, "TITLE") == 0) {
                        title = tag_value;
                    } else if (strcmp(tag_type, "GAME") == 0) {
                        game = tag_value;
                    } else if (strcmp(tag_type, "ARTIST") == 0) {
                        artist = tag_value;
                    }
                }

                // Populate metadata fields separately
                if (title != nullptr && title[0] != '\0') {
                    strncpy(info->song_name, title, sizeof(info->song_name) - 1);
                    info->song_name[sizeof(info->song_name) - 1] = '\0';
                }
                if (game != nullptr && game[0] != '\0') {
                    strncpy(info->game_name, game, sizeof(info->game_name) - 1);
                    info->game_name[sizeof(info->game_name) - 1] = '\0';
                }
                if (artist != nullptr && artist[0] != '\0') {
                    strncpy(info->artist_name, artist, sizeof(info->artist_name) - 1);
                    info->artist_name[sizeof(info->artist_name) - 1] = '\0';
                }
            }
        }
    }

#ifdef HAS_VGM_PATTERN
    if (data->pattern != nullptr) {
        info->num_channels = static_cast<uint8_t>(data->pattern->channel_count);
        info->num_patterns = 1;          // VGM has one "pattern" (the entire file)
        info->channels_synchronized = 0; // Per-channel scrolling - channels NOT synchronized

        // Pass native pattern data for direct VGM rendering
        info->native_pattern_data = data->pattern;

        // Report current playback position in samples
        if (data->player != nullptr) {
            double current_time = data->player->GetCurTime(PLAYTIME_TIME_FILE);
            info->current_sample = static_cast<uint32_t>(current_time * 44100.0);
        }

        // Copy channel names and per-channel info
        for (uint32_t i = 0; i < data->pattern->channel_count && i < RV_MAX_CHANNELS; i++) {
            if (data->pattern->channel_info != nullptr) {
                strncpy(info->channels[i].name, data->pattern->channel_info[i].name,
                        sizeof(info->channels[i].name) - 1);
            }
            // Per-channel row count and current row position
            if (data->pattern->channels != nullptr) {
                info->channels[i].num_rows = data->pattern->channels[i].row_count;
                // Calculate current row for this channel based on sample position
                info->channels[i].current_row = vgm_channel_find_row(&data->pattern->channels[i], info->current_sample);
            }
        }

        // Set main current_row using first channel with data (for compatibility with simple displays)
        // Also find max row count across all channels for rows_per_pattern
        uint32_t max_rows = 64;
        for (uint32_t i = 0; i < data->pattern->channel_count && i < RV_MAX_CHANNELS; i++) {
            if (data->pattern->channels != nullptr && data->pattern->channels[i].row_count > 0) {
                if (info->current_row == 0) {
                    info->current_row = static_cast<uint16_t>(info->channels[i].current_row);
                }
                if (data->pattern->channels[i].row_count > max_rows) {
                    max_rows = data->pattern->channels[i].row_count;
                }
            }
        }
        info->rows_per_pattern = static_cast<uint16_t>(max_rows > 65535 ? 65535 : max_rows);

        return 0;
    }
#endif

    return -1; // No pattern data available
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int libvgm_get_pattern_cell(void* user_data, int pattern, int row, int channel, RVPatternCell* cell) {
    (void)pattern; // VGM only has one "pattern"
    (void)row;
    (void)channel;
    LibvgmData* data = static_cast<LibvgmData*>(user_data);

    if (data == nullptr || cell == nullptr) {
        return -1;
    }

    memset(cell, 0, sizeof(RVPatternCell));

#ifdef HAS_VGM_PATTERN
    if (data->pattern == nullptr) {
        return -1;
    }

    if (channel < 0 || static_cast<uint32_t>(channel) >= data->pattern->channel_count) {
        return -1;
    }

    // Get the per-channel pattern
    const VgmChannelPattern* ch_pattern = &data->pattern->channels[channel];
    if (row < 0 || static_cast<uint32_t>(row) >= ch_pattern->row_count) {
        return -1;
    }

    const VgmPatternCell* vgm_cell = &ch_pattern->rows[row].cell;

    if (vgm_cell->has_note) {
        if (vgm_cell->type == VGM_NOTE_ON || vgm_cell->type == VGM_NOTE_CHANGE) {
            // Convert MIDI note to tracker note format (1 = C-0, etc.)
            // MIDI note 60 = C-4, tracker note 1 = C-0
            // So: tracker_note = midi_note - 60 + 48 + 1 = midi_note - 11
            // But clamp to valid range 1-96
            int tracker_note = vgm_cell->note - 11;
            if (tracker_note < 1)
                tracker_note = 1;
            if (tracker_note > 96)
                tracker_note = 96;
            cell->note = static_cast<uint8_t>(tracker_note);
            cell->volume = vgm_cell->velocity;
        } else if (vgm_cell->type == VGM_NOTE_OFF) {
            // Note-off: use special value (depends on format, often 97 or 0xFF)
            cell->note = 97; // Common "note off" value
        }
    }

    // Copy effect data if present
    if (vgm_cell->has_effect) {
        // Map VGM effect types to tracker effect commands
        // Using 'V' (0x16) for volume effect, similar to MOD/XM format
        if (vgm_cell->effect_type == VGM_EFFECT_VOLUME) {
            cell->effect = 'V'; // Volume effect command
            cell->effect_param = vgm_cell->effect_value;
        }
    }

    return 0;
#else
    return -1;
#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int libvgm_get_pattern_num_rows(void* user_data, int pattern) {
    (void)pattern; // VGM only has one "pattern"
    LibvgmData* data = static_cast<LibvgmData*>(user_data);

    if (data == nullptr) {
        return 0;
    }

#ifdef HAS_VGM_PATTERN
    if (data->pattern == nullptr) {
        return 0;
    }

    // For per-channel patterns, return the max row count among all channels
    uint32_t max_rows = 0;
    for (uint32_t ch = 0; ch < data->pattern->channel_count; ch++) {
        if (data->pattern->channels[ch].row_count > max_rows) {
            max_rows = data->pattern->channels[ch].row_count;
        }
    }
    return static_cast<int>(max_rows);
#else
    return 0;
#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void libvgm_static_init(const RVService* service_api) {
    rv_init_log_api(service_api);
    rv_init_io_api(service_api);
    rv_init_metadata_api(service_api);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Scope visualization - uses real per-channel audio from chip emulators

static uint32_t libvgm_get_scope_data(void* user_data, int channel, float* buffer, uint32_t num_samples) {
    LibvgmData* data = static_cast<LibvgmData*>(user_data);
    if (data == nullptr || buffer == nullptr || data->player == nullptr) {
        return 0;
    }

    if (channel < 0) {
        return 0;
    }

    // Auto-enable scope capture on first call
    if (!data->scope_enabled) {
        data->player->SetScopeEnabled(true);
        data->scope_enabled = true;
    }

    // Get scope data from player (which forwards to chip emulators)
    return data->player->GetScopeData(static_cast<uint8_t>(channel), buffer, num_samples);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t libvgm_get_scope_channel_names(void* user_data, const char** names, uint32_t max_channels) {
    LibvgmData* data = static_cast<LibvgmData*>(user_data);
    if (data == nullptr || data->player == nullptr)
        return 0;

    uint32_t count = data->player->GetScopeChannelCount();
    if (count > max_channels)
        count = max_channels;
    for (uint32_t i = 0; i < count; i++) {
        names[i] = data->player->GetScopeChannelName(static_cast<uint8_t>(i));
    }
    return count;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVPlaybackPlugin g_libvgm_plugin = {
    RV_PLAYBACK_PLUGIN_API_VERSION,
    "libvgm",
    "0.1.0",
    "libvgm",
    libvgm_probe_can_play,
    libvgm_supported_extensions,
    libvgm_create,
    libvgm_destroy,
    libvgm_event,
    libvgm_open,
    libvgm_close,
    libvgm_read_data,
    libvgm_seek,
    libvgm_metadata,
    libvgm_static_init,
    nullptr, // settings_updated

    // Tracker visualization API - VGM pattern extraction
    libvgm_get_tracker_info,
    libvgm_get_pattern_cell,
    libvgm_get_pattern_num_rows,

    // Scope visualization API - real per-channel audio from chip emulators
    libvgm_get_scope_data,
    nullptr, // static_destroy
    libvgm_get_scope_channel_names,
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

extern "C" RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_libvgm_plugin;
}
