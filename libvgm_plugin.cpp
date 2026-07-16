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
#endif
    // ponytail: scope enable state lives in the player, not mirrored here.
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

    if (len < 8 || event_data == nullptr || data == nullptr) {
        return;
    }

    // Legacy VU side-channel; per-channel playheads now come from get_channel_rows.
    memset(event_data, 0, 8);
    event_data[0] = data->vu_left;
    event_data[1] = data->vu_right;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Visualization API (per-channel scrolling; the whole register stream is known at open)

#define LIBVGM_COLUMN_COUNT 4

#ifdef HAS_VGM_PATTERN

static void libvgm_set_cell(RVPatternCell* cell, uint32_t raw, const char* text) {
    cell->raw = raw;
    memset(cell->text, 0, sizeof(cell->text));
    if (text != nullptr) {
        strncpy((char*)cell->text, text, sizeof(cell->text) - 1);
    }
}

static void libvgm_note_name(uint8_t midi, char* out, size_t out_size) {
    static const char* names[12] = { "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-" };
    snprintf(out, out_size, "%s%d", names[midi % 12], (int)midi / 12 - 1); // MIDI 60 = C-4
}

// Render LIBVGM_COLUMN_COUNT cells (Note, Vol, Eff, Prm) for one (channel, row). Each cell's
// `raw` carries the numeric value and `text` the rendered string, so the effect encoding is
// standardized at the boundary: the volume command lives in `raw` as VGM_EFFECT_VOLUME, the
// letter 'V' only in `text`.
static void libvgm_fill_row(const VgmChannelPattern* chp, uint32_t row, RVPatternCell* out) {
    for (int c = 0; c < LIBVGM_COLUMN_COUNT; c++) {
        libvgm_set_cell(&out[c], 0, "..");
    }
    if (chp == nullptr || row >= chp->row_count) {
        return;
    }
    const VgmPatternCell* cell = &chp->rows[row].cell;
    char buf[8];

    if (cell->has_note) {
        if (cell->type == VGM_NOTE_OFF) {
            libvgm_set_cell(&out[0], 0xFF, "==="); // note-off: standardized raw 0xFF
        } else {
            libvgm_note_name(cell->note, buf, sizeof(buf));
            libvgm_set_cell(&out[0], cell->note, buf);
            if (cell->velocity != 0) {
                snprintf(buf, sizeof(buf), "%02X", cell->velocity);
                libvgm_set_cell(&out[1], cell->velocity, buf);
            }
        }
    }

    if (cell->has_effect && cell->effect_type == VGM_EFFECT_VOLUME) {
        libvgm_set_cell(&out[2], VGM_EFFECT_VOLUME, "V");
        snprintf(buf, sizeof(buf), "%02X", cell->effect_value);
        libvgm_set_cell(&out[3], cell->effect_value, buf);
    }
}

#endif // HAS_VGM_PATTERN

static bool libvgm_get_structure(void* user_data, RVVizInfo* out) {
    LibvgmData* data = static_cast<LibvgmData*>(user_data);
    if (data == nullptr || out == nullptr) {
        return false;
    }
    uint32_t caps = 0;
    out->scroll_mode = RVScrollMode_PerChannel;
    out->pattern_channel_count = 0;
    out->column_count = 0;
#ifdef HAS_VGM_PATTERN
    if (data->pattern != nullptr && data->pattern->channel_count > 0) {
        // The whole VGM register stream is parsed at open, so every row is known up front.
        caps |= RVVizCaps_PatternCells | RVVizCaps_WholeSongKnown;
        out->pattern_channel_count = data->pattern->channel_count;
        out->column_count = LIBVGM_COLUMN_COUNT;
    }
#endif
    uint32_t scope_channels = data->player != nullptr ? data->player->GetScopeChannelCount() : 0;
    if (scope_channels > 0) {
        caps |= RVVizCaps_Scope;
    }
    out->caps = caps;
    out->scope_channel_count = scope_channels;
    return caps != 0;
}

static uint32_t libvgm_get_columns(void* user_data, RVColumnDesc* out, uint32_t cap) {
    (void)user_data;
#ifdef HAS_VGM_PATTERN
    static const struct {
        const char* label;
        uint8_t width;
        RVColumnKind kind;
    } cols[LIBVGM_COLUMN_COUNT] = {
        { "Note", 3, RVColumnKind_Note }, { "Vol", 2, RVColumnKind_Volume },
        { "Eff", 1, RVColumnKind_Effect }, { "Prm", 2, RVColumnKind_Param },
    };
    uint32_t n = cap < LIBVGM_COLUMN_COUNT ? cap : LIBVGM_COLUMN_COUNT;
    for (uint32_t i = 0; i < n; i++) {
        memset(out[i].label, 0, sizeof(out[i].label));
        strncpy((char*)out[i].label, cols[i].label, sizeof(out[i].label) - 1);
        out[i].char_width = cols[i].width;
        out[i].kind = cols[i].kind;
    }
    return n;
#else
    (void)out;
    (void)cap;
    return 0;
#endif
}

static uint32_t libvgm_get_pattern_channels(void* user_data, RVChannelDesc* out, uint32_t cap) {
#ifdef HAS_VGM_PATTERN
    LibvgmData* data = static_cast<LibvgmData*>(user_data);
    if (data == nullptr || data->pattern == nullptr) {
        return 0;
    }
    uint32_t count = data->pattern->channel_count;
    if (count > cap) {
        count = cap;
    }
    for (uint32_t i = 0; i < count; i++) {
        memset(out[i].name, 0, sizeof(out[i].name));
        const char* name = data->pattern->channel_info != nullptr ? data->pattern->channel_info[i].name : nullptr;
        if (name != nullptr) {
            strncpy((char*)out[i].name, name, sizeof(out[i].name) - 1);
        } else {
            snprintf((char*)out[i].name, sizeof(out[i].name), "Ch %u", i + 1);
        }
        out[i].scope_width = 0;
    }
    return count;
#else
    (void)user_data;
    (void)out;
    (void)cap;
    return 0;
#endif
}

static uint32_t libvgm_get_scope_channels(void* user_data, RVChannelDesc* out, uint32_t cap) {
    LibvgmData* data = static_cast<LibvgmData*>(user_data);
    if (data == nullptr || data->player == nullptr) {
        return 0;
    }
    uint32_t count = data->player->GetScopeChannelCount();
    if (count > cap) {
        count = cap;
    }
    for (uint32_t i = 0; i < count; i++) {
        memset(out[i].name, 0, sizeof(out[i].name));
        const char* name = data->player->GetScopeChannelName(static_cast<uint8_t>(i));
        if (name != nullptr) {
            strncpy((char*)out[i].name, name, sizeof(out[i].name) - 1);
        }
        out[i].scope_width = 1; // mono per voice
    }
    return count;
}

static bool libvgm_get_position(void* user_data, RVTrackerPosition* out) {
#ifdef HAS_VGM_PATTERN
    LibvgmData* data = static_cast<LibvgmData*>(user_data);
    if (data == nullptr || data->pattern == nullptr || out == nullptr) {
        return false;
    }
    uint32_t max_rows = 0;
    for (uint32_t ch = 0; ch < data->pattern->channel_count; ch++) {
        if (data->pattern->channels[ch].row_count > max_rows) {
            max_rows = data->pattern->channels[ch].row_count;
        }
    }
    out->order = 0;
    out->pattern = 0;
    out->row = 0;
    out->window_lo = 0;
    out->window_hi = max_rows; // whole song; per-channel playheads come from get_channel_rows
    return max_rows > 0;
#else
    (void)user_data;
    (void)out;
    return false;
#endif
}

static uint32_t libvgm_get_channel_rows(void* user_data, uint32_t* out, uint32_t cap) {
#ifdef HAS_VGM_PATTERN
    LibvgmData* data = static_cast<LibvgmData*>(user_data);
    if (data == nullptr || data->pattern == nullptr || out == nullptr || data->player == nullptr) {
        return 0;
    }
    // The quantizer indexes rows by VGM sample position (44100 Hz file clock).
    uint32_t sample = static_cast<uint32_t>(data->player->GetCurTime(PLAYTIME_TIME_FILE) * 44100.0);
    uint32_t count = data->pattern->channel_count;
    if (count > cap) {
        count = cap;
    }
    for (uint32_t i = 0; i < count; i++) {
        out[i] = vgm_channel_find_row(&data->pattern->channels[i], sample);
    }
    return count;
#else
    (void)user_data;
    (void)out;
    (void)cap;
    return 0;
#endif
}

static uint32_t libvgm_get_cells(void* user_data, int32_t channel, uint32_t row_lo, uint32_t row_hi, RVPatternCell* out,
                                 uint32_t cap) {
#ifdef HAS_VGM_PATTERN
    LibvgmData* data = static_cast<LibvgmData*>(user_data);
    if (data == nullptr || data->pattern == nullptr || out == nullptr) {
        return 0;
    }
    int num_channels = static_cast<int>(data->pattern->channel_count);
    if (num_channels <= 0) {
        return 0;
    }
    int ch_start = channel < 0 ? 0 : channel;
    int ch_end = channel < 0 ? num_channels : channel + 1;
    if (ch_start >= num_channels) {
        return 0;
    }
    if (ch_end > num_channels) {
        ch_end = num_channels;
    }
    // Rectangular row-major grid (row -> channel -> column). Rows past a short channel's end
    // yield empty cells so the host can index a fixed stride across channels.
    uint32_t written = 0;
    for (uint32_t row = row_lo; row < row_hi; row++) {
        for (int ch = ch_start; ch < ch_end; ch++) {
            if (written + LIBVGM_COLUMN_COUNT > cap) {
                return written;
            }
            libvgm_fill_row(&data->pattern->channels[ch], row, &out[written]);
            written += LIBVGM_COLUMN_COUNT;
        }
    }
    return written;
#else
    (void)user_data;
    (void)channel;
    (void)row_lo;
    (void)row_hi;
    (void)out;
    (void)cap;
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
// Scope visualization - real per-channel audio from chip emulators

static void libvgm_set_scope_enabled(void* user_data, bool on) {
    LibvgmData* data = static_cast<LibvgmData*>(user_data);
    if (data == nullptr || data->player == nullptr) {
        return;
    }
    data->player->SetScopeEnabled(on);
}

static uint32_t libvgm_get_scope_samples(void* user_data, int32_t channel, float* out, uint32_t cap) {
    LibvgmData* data = static_cast<LibvgmData*>(user_data);
    if (data == nullptr || out == nullptr || data->player == nullptr || channel < 0) {
        return 0;
    }
    // Capture is gated by set_scope_enabled; the player yields silence until then.
    return data->player->GetScopeData(static_cast<uint8_t>(channel), out, cap);
}

// VU is reported through the legacy event side-channel (vu_left/vu_right); no value-semantic
// VU surface is wired here yet, so this slot reports none (matches the other migrated plugins).
static uint32_t libvgm_get_vu(void* user_data, float* out, uint32_t cap) {
    (void)user_data;
    (void)out;
    (void)cap;
    return 0;
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
    nullptr, // static_destroy
    libvgm_get_structure,
    libvgm_get_columns,
    libvgm_get_pattern_channels,
    libvgm_get_scope_channels,
    libvgm_get_position,
    libvgm_get_channel_rows,
    libvgm_get_cells,
    libvgm_set_scope_enabled,
    libvgm_get_scope_samples,
    libvgm_get_vu,
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

extern "C" RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_libvgm_plugin;
}
