///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// VGM Parser Implementation
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "vgm_parser.h"
#include "vgm_alloc.h"
#include <string.h>

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// VGM Header structure (packed to match file format)
// Reference: https://vgmrips.net/wiki/VGM_Specification

#pragma pack(push, 1)
typedef struct {
    u32 magic;              // 0x00: "Vgm " (0x206D6756)
    u32 eof_offset;         // 0x04: Relative offset to end of file
    u32 version;            // 0x08: Version number (BCD)
    u32 sn76489_clock;      // 0x0C: SN76489 clock
    u32 ym2413_clock;       // 0x10: YM2413 clock
    u32 gd3_offset;         // 0x14: Relative offset to GD3 tag
    u32 total_samples;      // 0x18: Total samples
    u32 loop_offset;        // 0x1C: Relative offset to loop point
    u32 loop_samples;       // 0x20: Number of samples in loop
    u32 rate;               // 0x24: Rate (v1.01+)
    u16 sn76489_feedback;   // 0x28: SN76489 feedback (v1.10+)
    u8 sn76489_shift_width; // 0x2A: SN76489 shift register width
    u8 sn76489_flags;       // 0x2B: SN76489 flags
    u32 ym2612_clock;       // 0x2C: YM2612 clock (v1.10+)
    u32 ym2151_clock;       // 0x30: YM2151 clock (v1.10+)
    u32 vgm_data_offset;    // 0x34: Relative offset to VGM data (v1.50+)
    u32 segapcm_clock;      // 0x38: SegaPCM clock (v1.51+)
    u32 segapcm_interface;  // 0x3C: SegaPCM interface register
    // Extended header fields (v1.51+, offset 0x40+)
    u32 rf5c68_clock;   // 0x40
    u32 ym2203_clock;   // 0x44
    u32 ym2608_clock;   // 0x48
    u32 ym2610_clock;   // 0x4C
    u32 ym3812_clock;   // 0x50
    u32 ym3526_clock;   // 0x54
    u32 y8950_clock;    // 0x58
    u32 ymf262_clock;   // 0x5C
    u32 ymf278b_clock;  // 0x60
    u32 ymf271_clock;   // 0x64
    u32 ymz280b_clock;  // 0x68
    u32 rf5c164_clock;  // 0x6C
    u32 pwm_clock;      // 0x70
    u32 ay8910_clock;   // 0x74
    u8 ay8910_type;     // 0x78
    u8 ay8910_flags;    // 0x79
    u8 ym2203_ay_flags; // 0x7A
    u8 ym2608_ay_flags; // 0x7B
    u8 volume_modifier; // 0x7C (v1.60+)
    u8 reserved_7d;     // 0x7D
    u8 loop_base;       // 0x7E (v1.60+)
    u8 loop_modifier;   // 0x7F (v1.51+)
    // v1.61+ fields at 0x80+
    u32 gameboy_clock;  // 0x80
    u32 nesapu_clock;   // 0x84
    u32 multipcm_clock; // 0x88
    u32 upd7759_clock;  // 0x8C
    u32 okim6258_clock; // 0x90
    u8 okim6258_flags;  // 0x94
    u8 k054539_flags;   // 0x95
    u8 c140_type;       // 0x96
    u8 reserved_97;     // 0x97
    u32 okim6295_clock; // 0x98
    u32 k051649_clock;  // 0x9C
    u32 k054539_clock;  // 0xA0
    u32 huc6280_clock;  // 0xA4
    u32 c140_clock;     // 0xA8
    u32 k053260_clock;  // 0xAC
    u32 pokey_clock;    // 0xB0
    u32 qsound_clock;   // 0xB4
    // More fields exist but we stop here for now
} VgmRawHeader;
#pragma pack(pop)

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: read little-endian values (handles unaligned access safely)

static inline u16 read_u16_le(const u8* p) {
    return (u16)p[0] | ((u16)p[1] << 8);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static inline u32 read_u32_le(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Chip names

static const char* s_chip_names[VGM_CHIP_COUNT] = {
    [VGM_CHIP_SN76489] = "SN76489",   [VGM_CHIP_YM2413] = "YM2413",   [VGM_CHIP_YM2612] = "YM2612",
    [VGM_CHIP_YM2151] = "YM2151",     [VGM_CHIP_SEGAPCM] = "SegaPCM", [VGM_CHIP_RF5C68] = "RF5C68",
    [VGM_CHIP_YM2203] = "YM2203",     [VGM_CHIP_YM2608] = "YM2608",   [VGM_CHIP_YM2610] = "YM2610",
    [VGM_CHIP_YM3812] = "YM3812",     [VGM_CHIP_YM3526] = "YM3526",   [VGM_CHIP_Y8950] = "Y8950",
    [VGM_CHIP_YMF262] = "YMF262",     [VGM_CHIP_YMF278B] = "YMF278B", [VGM_CHIP_YMF271] = "YMF271",
    [VGM_CHIP_YMZ280B] = "YMZ280B",   [VGM_CHIP_RF5C164] = "RF5C164", [VGM_CHIP_PWM] = "PWM",
    [VGM_CHIP_AY8910] = "AY8910",     [VGM_CHIP_GAMEBOY] = "GameBoy", [VGM_CHIP_NESAPU] = "NES APU",
    [VGM_CHIP_MULTIPCM] = "MultiPCM", [VGM_CHIP_UPD7759] = "uPD7759", [VGM_CHIP_OKIM6258] = "OKIM6258",
    [VGM_CHIP_OKIM6295] = "OKIM6295", [VGM_CHIP_K051649] = "K051649", [VGM_CHIP_K054539] = "K054539",
    [VGM_CHIP_HUC6280] = "HuC6280",   [VGM_CHIP_C140] = "C140",       [VGM_CHIP_K053260] = "K053260",
    [VGM_CHIP_POKEY] = "POKEY",       [VGM_CHIP_QSOUND] = "QSound",
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const char* vgm_chip_name(VgmChipId chip_id) {
    if (chip_id >= VGM_CHIP_COUNT) {
        return "Unknown";
    }
    return s_chip_names[chip_id];
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Error messages

static const char* s_parse_status_strings[] = {
    [VGM_PARSE_OK] = "OK",
    [VGM_PARSE_ERROR_NULL_BUFFER] = "Null buffer provided",
    [VGM_PARSE_ERROR_TOO_SMALL] = "Buffer too small for VGM header",
    [VGM_PARSE_ERROR_INVALID_MAGIC] = "Invalid VGM magic (expected 'Vgm ')",
    [VGM_PARSE_ERROR_UNSUPPORTED_VERSION] = "Unsupported VGM version",
    [VGM_PARSE_ERROR_INVALID_DATA_OFFSET] = "Invalid VGM data offset",
    [VGM_PARSE_ERROR_ALLOCATION_FAILED] = "Memory allocation failed",
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const char* vgm_parse_status_string(VgmParseStatus status) {
    if (status > VGM_PARSE_ERROR_ALLOCATION_FAILED) {
        return "Unknown error";
    }
    return s_parse_status_strings[status];
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: set chip info from clock value

static void set_chip_info(VgmChipInfo* info, u32 clock) {
    if (clock == 0) {
        info->present = false;
        info->clock = 0;
        info->dual_chip = false;
    } else {
        info->present = true;
        info->dual_chip = (clock & 0x40000000) != 0;
        info->clock = clock & 0x3FFFFFFF;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Parse VGM file

VgmParseResult vgm_parse(VgmAllocator* alloc, const u8* buffer, u64 size) {
    VgmParseResult result = { 0 };

    // Validate input
    if (buffer == nullptr) {
        result.status = VGM_PARSE_ERROR_NULL_BUFFER;
        return result;
    }

    if (size < VGM_HEADER_MIN_SIZE) {
        result.status = VGM_PARSE_ERROR_TOO_SMALL;
        return result;
    }

    // Check magic
    u32 magic = read_u32_le(buffer);
    if (magic != VGM_MAGIC) {
        result.status = VGM_PARSE_ERROR_INVALID_MAGIC;
        return result;
    }

    // Read version
    u32 version = read_u32_le(buffer + 0x08);

    // Allocate VgmFile
    VgmFile* file = (VgmFile*)vgm_alloc(alloc, sizeof(VgmFile));
    if (file == nullptr) {
        result.status = VGM_PARSE_ERROR_ALLOCATION_FAILED;
        return result;
    }

    file->buffer = buffer;
    file->buffer_size = size;
    file->version = version;

    // Parse basic header fields (always present)
    file->total_samples = read_u32_le(buffer + 0x18);
    file->loop_samples = read_u32_le(buffer + 0x20);

    // GD3 offset is relative to position 0x14
    u32 gd3_rel = read_u32_le(buffer + 0x14);
    file->gd3_offset = (gd3_rel > 0) ? (0x14 + gd3_rel) : 0;

    // Loop offset is relative to position 0x1C
    u32 loop_rel = read_u32_le(buffer + 0x1C);
    file->loop_offset = (loop_rel > 0) ? (0x1C + loop_rel) : 0;

    // Parse chip clocks (v1.00 base chips)
    set_chip_info(&file->chips[VGM_CHIP_SN76489], read_u32_le(buffer + 0x0C));
    set_chip_info(&file->chips[VGM_CHIP_YM2413], read_u32_le(buffer + 0x10));

    // v1.10+ chips
    if (version >= 0x110 && size >= 0x34) {
        set_chip_info(&file->chips[VGM_CHIP_YM2612], read_u32_le(buffer + 0x2C));
        set_chip_info(&file->chips[VGM_CHIP_YM2151], read_u32_le(buffer + 0x30));
    }

    // Determine VGM data offset
    u32 data_offset;
    if (version >= 0x150 && size >= 0x38) {
        u32 data_rel = read_u32_le(buffer + 0x34);
        data_offset = (data_rel > 0) ? (0x34 + data_rel) : 0x40;
    } else {
        // Pre-1.50 files have data immediately after 0x40 header
        data_offset = 0x40;
    }

    // Validate data offset
    if (data_offset >= size) {
        result.status = VGM_PARSE_ERROR_INVALID_DATA_OFFSET;
        return result;
    }

    file->data = buffer + data_offset;
    file->data_size = size - data_offset;

    // v1.51+ extended chips
    if (version >= 0x151 && size >= 0xB8) {
        set_chip_info(&file->chips[VGM_CHIP_SEGAPCM], read_u32_le(buffer + 0x38));
        set_chip_info(&file->chips[VGM_CHIP_RF5C68], read_u32_le(buffer + 0x40));
        set_chip_info(&file->chips[VGM_CHIP_YM2203], read_u32_le(buffer + 0x44));
        set_chip_info(&file->chips[VGM_CHIP_YM2608], read_u32_le(buffer + 0x48));
        set_chip_info(&file->chips[VGM_CHIP_YM2610], read_u32_le(buffer + 0x4C));
        set_chip_info(&file->chips[VGM_CHIP_YM3812], read_u32_le(buffer + 0x50));
        set_chip_info(&file->chips[VGM_CHIP_YM3526], read_u32_le(buffer + 0x54));
        set_chip_info(&file->chips[VGM_CHIP_Y8950], read_u32_le(buffer + 0x58));
        set_chip_info(&file->chips[VGM_CHIP_YMF262], read_u32_le(buffer + 0x5C));
        set_chip_info(&file->chips[VGM_CHIP_YMF278B], read_u32_le(buffer + 0x60));
        set_chip_info(&file->chips[VGM_CHIP_YMF271], read_u32_le(buffer + 0x64));
        set_chip_info(&file->chips[VGM_CHIP_YMZ280B], read_u32_le(buffer + 0x68));
        set_chip_info(&file->chips[VGM_CHIP_RF5C164], read_u32_le(buffer + 0x6C));
        set_chip_info(&file->chips[VGM_CHIP_PWM], read_u32_le(buffer + 0x70));
        set_chip_info(&file->chips[VGM_CHIP_AY8910], read_u32_le(buffer + 0x74));
    }

    // v1.61+ additional chips
    if (version >= 0x161 && size >= 0xB8) {
        set_chip_info(&file->chips[VGM_CHIP_GAMEBOY], read_u32_le(buffer + 0x80));
        set_chip_info(&file->chips[VGM_CHIP_NESAPU], read_u32_le(buffer + 0x84));
        set_chip_info(&file->chips[VGM_CHIP_MULTIPCM], read_u32_le(buffer + 0x88));
        set_chip_info(&file->chips[VGM_CHIP_UPD7759], read_u32_le(buffer + 0x8C));
        set_chip_info(&file->chips[VGM_CHIP_OKIM6258], read_u32_le(buffer + 0x90));
        set_chip_info(&file->chips[VGM_CHIP_OKIM6295], read_u32_le(buffer + 0x98));
        set_chip_info(&file->chips[VGM_CHIP_K051649], read_u32_le(buffer + 0x9C));
        set_chip_info(&file->chips[VGM_CHIP_K054539], read_u32_le(buffer + 0xA0));
        set_chip_info(&file->chips[VGM_CHIP_HUC6280], read_u32_le(buffer + 0xA4));
        set_chip_info(&file->chips[VGM_CHIP_C140], read_u32_le(buffer + 0xA8));
        set_chip_info(&file->chips[VGM_CHIP_K053260], read_u32_le(buffer + 0xAC));
        set_chip_info(&file->chips[VGM_CHIP_POKEY], read_u32_le(buffer + 0xB0));
        set_chip_info(&file->chips[VGM_CHIP_QSOUND], read_u32_le(buffer + 0xB4));
    }

    result.file = file;
    result.status = VGM_PARSE_OK;
    return result;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Command iterator

VgmIterator vgm_iterator_create(const VgmFile* file) {
    VgmIterator iter = { 0 };
    if (file != nullptr && file->data != nullptr) {
        iter.current = file->data;
        iter.end = file->data + file->data_size;
        iter.sample_position = 0;
        iter.finished = false;

        // Set loop point if present
        if (file->loop_offset > 0 && file->loop_offset < file->buffer_size) {
            iter.loop_point = file->buffer + file->loop_offset;
        } else {
            iter.loop_point = nullptr;
        }
    } else {
        iter.finished = true;
    }
    return iter;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void vgm_iterator_reset(VgmIterator* iter, const VgmFile* file) {
    *iter = vgm_iterator_create(file);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Get next command

bool vgm_iterator_next(VgmIterator* iter, VgmCommand* out_cmd) {
    if (iter->finished || iter->current >= iter->end) {
        iter->finished = true;
        return false;
    }

    // Initialize output command
    memset(out_cmd, 0, sizeof(VgmCommand));
    out_cmd->sample_time = iter->sample_position;

    u8 cmd = *iter->current++;

    switch (cmd) {
        // Wait commands
        case 0x61: // Wait n samples (16-bit)
            if (iter->current + 2 > iter->end) {
                iter->finished = true;
                return false;
            }
            out_cmd->type = VGM_CMD_WAIT;
            out_cmd->wait_samples = read_u16_le(iter->current);
            iter->current += 2;
            iter->sample_position += out_cmd->wait_samples;
            break;

        case 0x62: // Wait 735 samples (60Hz NTSC frame)
            out_cmd->type = VGM_CMD_WAIT;
            out_cmd->wait_samples = 735;
            iter->sample_position += 735;
            break;

        case 0x63: // Wait 882 samples (50Hz PAL frame)
            out_cmd->type = VGM_CMD_WAIT;
            out_cmd->wait_samples = 882;
            iter->sample_position += 882;
            break;

        case 0x66: // End of sound data
            out_cmd->type = VGM_CMD_END;
            iter->finished = true;
            break;

        // 0x70-0x7F: Wait n+1 samples (n = low nibble)
        case 0x70:
        case 0x71:
        case 0x72:
        case 0x73:
        case 0x74:
        case 0x75:
        case 0x76:
        case 0x77:
        case 0x78:
        case 0x79:
        case 0x7A:
        case 0x7B:
        case 0x7C:
        case 0x7D:
        case 0x7E:
        case 0x7F:
            out_cmd->type = VGM_CMD_WAIT;
            out_cmd->wait_samples = (cmd & 0x0F) + 1;
            iter->sample_position += out_cmd->wait_samples;
            break;

        // PSG (SN76489)
        case 0x50:
            if (iter->current + 1 > iter->end) {
                iter->finished = true;
                return false;
            }
            out_cmd->type = VGM_CMD_WRITE_PSG;
            out_cmd->value = *iter->current++;
            break;

        // YM2413
        case 0x51:
            if (iter->current + 2 > iter->end) {
                iter->finished = true;
                return false;
            }
            out_cmd->type = VGM_CMD_WRITE_YM2413;
            out_cmd->reg = *iter->current++;
            out_cmd->value = *iter->current++;
            break;

        // YM2612 port 0
        case 0x52:
            if (iter->current + 2 > iter->end) {
                iter->finished = true;
                return false;
            }
            out_cmd->type = VGM_CMD_WRITE_YM2612_P0;
            out_cmd->port = 0;
            out_cmd->reg = *iter->current++;
            out_cmd->value = *iter->current++;
            break;

        // YM2612 port 1
        case 0x53:
            if (iter->current + 2 > iter->end) {
                iter->finished = true;
                return false;
            }
            out_cmd->type = VGM_CMD_WRITE_YM2612_P1;
            out_cmd->port = 1;
            out_cmd->reg = *iter->current++;
            out_cmd->value = *iter->current++;
            break;

        // YM2151
        case 0x54:
            if (iter->current + 2 > iter->end) {
                iter->finished = true;
                return false;
            }
            out_cmd->type = VGM_CMD_WRITE_YM2151;
            out_cmd->reg = *iter->current++;
            out_cmd->value = *iter->current++;
            break;

        // YM2203
        case 0x55:
            if (iter->current + 2 > iter->end) {
                iter->finished = true;
                return false;
            }
            out_cmd->type = VGM_CMD_WRITE_YM2203;
            out_cmd->reg = *iter->current++;
            out_cmd->value = *iter->current++;
            break;

        // YM2608 port 0
        case 0x56:
            if (iter->current + 2 > iter->end) {
                iter->finished = true;
                return false;
            }
            out_cmd->type = VGM_CMD_WRITE_YM2608_P0;
            out_cmd->port = 0;
            out_cmd->reg = *iter->current++;
            out_cmd->value = *iter->current++;
            break;

        // YM2608 port 1
        case 0x57:
            if (iter->current + 2 > iter->end) {
                iter->finished = true;
                return false;
            }
            out_cmd->type = VGM_CMD_WRITE_YM2608_P1;
            out_cmd->port = 1;
            out_cmd->reg = *iter->current++;
            out_cmd->value = *iter->current++;
            break;

        // YM2610 port 0
        case 0x58:
            if (iter->current + 2 > iter->end) {
                iter->finished = true;
                return false;
            }
            out_cmd->type = VGM_CMD_WRITE_YM2610_P0;
            out_cmd->port = 0;
            out_cmd->reg = *iter->current++;
            out_cmd->value = *iter->current++;
            break;

        // YM2610 port 1
        case 0x59:
            if (iter->current + 2 > iter->end) {
                iter->finished = true;
                return false;
            }
            out_cmd->type = VGM_CMD_WRITE_YM2610_P1;
            out_cmd->port = 1;
            out_cmd->reg = *iter->current++;
            out_cmd->value = *iter->current++;
            break;

        // YM3812
        case 0x5A:
            if (iter->current + 2 > iter->end) {
                iter->finished = true;
                return false;
            }
            out_cmd->type = VGM_CMD_WRITE_YM3812;
            out_cmd->reg = *iter->current++;
            out_cmd->value = *iter->current++;
            break;

        // YM3526
        case 0x5B:
            if (iter->current + 2 > iter->end) {
                iter->finished = true;
                return false;
            }
            out_cmd->type = VGM_CMD_WRITE_YM3526;
            out_cmd->reg = *iter->current++;
            out_cmd->value = *iter->current++;
            break;

        // Y8950
        case 0x5C:
            if (iter->current + 2 > iter->end) {
                iter->finished = true;
                return false;
            }
            out_cmd->type = VGM_CMD_WRITE_Y8950;
            out_cmd->reg = *iter->current++;
            out_cmd->value = *iter->current++;
            break;

        // YMZ280B
        case 0x5D:
            if (iter->current + 2 > iter->end) {
                iter->finished = true;
                return false;
            }
            out_cmd->type = VGM_CMD_WRITE_YMZ280B;
            out_cmd->reg = *iter->current++;
            out_cmd->value = *iter->current++;
            break;

        // YMF262 port 0
        case 0x5E:
            if (iter->current + 2 > iter->end) {
                iter->finished = true;
                return false;
            }
            out_cmd->type = VGM_CMD_WRITE_YMF262_P0;
            out_cmd->port = 0;
            out_cmd->reg = *iter->current++;
            out_cmd->value = *iter->current++;
            break;

        // YMF262 port 1
        case 0x5F:
            if (iter->current + 2 > iter->end) {
                iter->finished = true;
                return false;
            }
            out_cmd->type = VGM_CMD_WRITE_YMF262_P1;
            out_cmd->port = 1;
            out_cmd->reg = *iter->current++;
            out_cmd->value = *iter->current++;
            break;

        // Data block
        case 0x67:
            if (iter->current + 6 > iter->end) {
                iter->finished = true;
                return false;
            }
            out_cmd->type = VGM_CMD_DATA_BLOCK;
            // Skip: 0x66 compatibility byte, type byte, size (4 bytes)
            iter->current++; // Skip 0x66
            iter->current++; // Skip type
            {
                u32 block_size = read_u32_le(iter->current);
                iter->current += 4;
                // Skip the data block content
                if (iter->current + block_size > iter->end) {
                    iter->finished = true;
                    return false;
                }
                iter->current += block_size;
            }
            break;

        // PCM RAM write
        case 0x68:
            if (iter->current + 11 > iter->end) {
                iter->finished = true;
                return false;
            }
            out_cmd->type = VGM_CMD_PCM_RAM_WRITE;
            // Skip: 0x66, chip type, read offset (3), write offset (3), size (3)
            iter->current += 11;
            break;

        // AY8910
        case 0xA0:
            if (iter->current + 2 > iter->end) {
                iter->finished = true;
                return false;
            }
            out_cmd->type = VGM_CMD_WRITE_AY8910;
            out_cmd->reg = *iter->current++;
            out_cmd->value = *iter->current++;
            break;

        // 0x80-0x8F: YM2612 port 0 write + wait (value from data bank)
        case 0x80:
        case 0x81:
        case 0x82:
        case 0x83:
        case 0x84:
        case 0x85:
        case 0x86:
        case 0x87:
        case 0x88:
        case 0x89:
        case 0x8A:
        case 0x8B:
        case 0x8C:
        case 0x8D:
        case 0x8E:
        case 0x8F:
            // These write to YM2612 DAC and wait n samples
            out_cmd->type = VGM_CMD_WRITE_YM2612_P0;
            out_cmd->port = 0;
            out_cmd->reg = 0x2A; // DAC register
            // Note: actual value comes from data block - we mark it as DAC write
            {
                u8 wait = cmd & 0x0F;
                if (wait > 0) {
                    iter->sample_position += wait;
                }
            }
            break;

        // 0xE0: Seek to offset in PCM data bank
        case 0xE0:
            if (iter->current + 4 > iter->end) {
                iter->finished = true;
                return false;
            }
            // Skip the 4-byte offset
            iter->current += 4;
            out_cmd->type = VGM_CMD_UNKNOWN;
            break;

        // 0x30-0x3F: Reserved single-byte commands
        case 0x30:
        case 0x31:
        case 0x32:
        case 0x33:
        case 0x34:
        case 0x35:
        case 0x36:
        case 0x37:
        case 0x38:
        case 0x39:
        case 0x3A:
        case 0x3B:
        case 0x3C:
        case 0x3D:
        case 0x3E:
        case 0x3F:
            if (iter->current + 1 > iter->end) {
                iter->finished = true;
                return false;
            }
            iter->current++; // Skip 1 byte
            out_cmd->type = VGM_CMD_UNKNOWN;
            break;

        // 0x40-0x4E: Reserved two-byte commands
        case 0x40:
        case 0x41:
        case 0x42:
        case 0x43:
        case 0x44:
        case 0x45:
        case 0x46:
        case 0x47:
        case 0x48:
        case 0x49:
        case 0x4A:
        case 0x4B:
        case 0x4C:
        case 0x4D:
        case 0x4E:
            if (iter->current + 2 > iter->end) {
                iter->finished = true;
                return false;
            }
            iter->current += 2; // Skip 2 bytes
            out_cmd->type = VGM_CMD_UNKNOWN;
            break;

        // 0x4F: GameGear stereo
        case 0x4F:
            if (iter->current + 1 > iter->end) {
                iter->finished = true;
                return false;
            }
            iter->current++; // Skip stereo byte
            out_cmd->type = VGM_CMD_UNKNOWN;
            break;

        // 0x90-0x95: DAC stream control
        case 0x90:
        case 0x91:
        case 0x92:
        case 0x93:
        case 0x94:
        case 0x95:
            // Variable length - handle based on command
            if (cmd == 0x90) {
                if (iter->current + 4 > iter->end) {
                    iter->finished = true;
                    return false;
                }
                iter->current += 4;
            } else if (cmd == 0x91) {
                if (iter->current + 4 > iter->end) {
                    iter->finished = true;
                    return false;
                }
                iter->current += 4;
            } else if (cmd == 0x92) {
                if (iter->current + 5 > iter->end) {
                    iter->finished = true;
                    return false;
                }
                iter->current += 5;
            } else if (cmd == 0x93) {
                if (iter->current + 10 > iter->end) {
                    iter->finished = true;
                    return false;
                }
                iter->current += 10;
            } else if (cmd == 0x94) {
                if (iter->current + 1 > iter->end) {
                    iter->finished = true;
                    return false;
                }
                iter->current += 1;
            } else if (cmd == 0x95) {
                if (iter->current + 4 > iter->end) {
                    iter->finished = true;
                    return false;
                }
                iter->current += 4;
            }
            out_cmd->type = VGM_CMD_UNKNOWN;
            break;

        default:
            // Handle other 0xBn, 0xCn, 0xDn commands (various chips)
            if ((cmd >= 0xB0 && cmd <= 0xBF) || (cmd >= 0xC0 && cmd <= 0xC8)) {
                // 2-byte commands
                if (iter->current + 2 > iter->end) {
                    iter->finished = true;
                    return false;
                }
                iter->current += 2;
            } else if (cmd >= 0xD0 && cmd <= 0xD6) {
                // 3-byte commands
                if (iter->current + 3 > iter->end) {
                    iter->finished = true;
                    return false;
                }
                iter->current += 3;
            } else if (cmd >= 0xE1 && cmd <= 0xFF) {
                // 4-byte commands
                if (iter->current + 4 > iter->end) {
                    iter->finished = true;
                    return false;
                }
                iter->current += 4;
            }
            out_cmd->type = VGM_CMD_UNKNOWN;
            break;
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Seek to sample position

void vgm_iterator_seek(VgmIterator* iter, const VgmFile* file, u32 target_sample) {
    // Reset to beginning
    vgm_iterator_reset(iter, file);

    // Iterate until we reach target
    VgmCommand cmd;
    while (!iter->finished && iter->sample_position < target_sample) {
        if (!vgm_iterator_next(iter, &cmd)) {
            break;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
