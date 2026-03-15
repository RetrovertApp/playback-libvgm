///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// VGM Parser - Video Game Music format parsing
//
// Parses VGM file headers and provides an iterator for the command stream.
// VGM is a sample-accurate logging format at 44100Hz that records register writes
// to various sound chips with timing information.
//
// Supported VGM versions: 1.00 - 1.71
// Reference: https://vgmrips.net/wiki/VGM_Specification
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

// Define nullptr for C11 compatibility
#ifndef __cplusplus
#if !defined(nullptr) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L)
#define nullptr ((void*)0)
#endif
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Forward declarations

typedef struct VgmAllocator VgmAllocator;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Constants

#define VGM_MAGIC 0x206D6756 // "Vgm " in little-endian
#define VGM_SAMPLE_RATE 44100
#define VGM_HEADER_MIN_SIZE 64

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Chip IDs

typedef enum VgmChipId {
    VGM_CHIP_SN76489 = 0,
    VGM_CHIP_YM2413,
    VGM_CHIP_YM2612,
    VGM_CHIP_YM2151,
    VGM_CHIP_SEGAPCM,
    VGM_CHIP_RF5C68,
    VGM_CHIP_YM2203,
    VGM_CHIP_YM2608,
    VGM_CHIP_YM2610,
    VGM_CHIP_YM3812,
    VGM_CHIP_YM3526,
    VGM_CHIP_Y8950,
    VGM_CHIP_YMF262,
    VGM_CHIP_YMF278B,
    VGM_CHIP_YMF271,
    VGM_CHIP_YMZ280B,
    VGM_CHIP_RF5C164,
    VGM_CHIP_PWM,
    VGM_CHIP_AY8910,
    VGM_CHIP_GAMEBOY,
    VGM_CHIP_NESAPU,
    VGM_CHIP_MULTIPCM,
    VGM_CHIP_UPD7759,
    VGM_CHIP_OKIM6258,
    VGM_CHIP_OKIM6295,
    VGM_CHIP_K051649,
    VGM_CHIP_K054539,
    VGM_CHIP_HUC6280,
    VGM_CHIP_C140,
    VGM_CHIP_K053260,
    VGM_CHIP_POKEY,
    VGM_CHIP_QSOUND,
    VGM_CHIP_COUNT
} VgmChipId;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Command types

typedef enum VgmCommandType {
    VGM_CMD_INVALID = 0,
    VGM_CMD_END,             // 0x66 - End of sound data
    VGM_CMD_WAIT,            // Wait N samples
    VGM_CMD_WRITE_PSG,       // 0x50 - SN76489 write
    VGM_CMD_WRITE_YM2413,    // 0x51 - YM2413 write
    VGM_CMD_WRITE_YM2612_P0, // 0x52 - YM2612 port 0 write
    VGM_CMD_WRITE_YM2612_P1, // 0x53 - YM2612 port 1 write
    VGM_CMD_WRITE_YM2151,    // 0x54 - YM2151 write
    VGM_CMD_WRITE_YM2203,    // 0x55 - YM2203 write
    VGM_CMD_WRITE_YM2608_P0, // 0x56 - YM2608 port 0 write
    VGM_CMD_WRITE_YM2608_P1, // 0x57 - YM2608 port 1 write
    VGM_CMD_WRITE_YM2610_P0, // 0x58 - YM2610 port 0 write
    VGM_CMD_WRITE_YM2610_P1, // 0x59 - YM2610 port 1 write
    VGM_CMD_WRITE_YM3812,    // 0x5A - YM3812 write
    VGM_CMD_WRITE_YM3526,    // 0x5B - YM3526 write
    VGM_CMD_WRITE_Y8950,     // 0x5C - Y8950 write
    VGM_CMD_WRITE_YMZ280B,   // 0x5D - YMZ280B write
    VGM_CMD_WRITE_YMF262_P0, // 0x5E - YMF262 port 0 write
    VGM_CMD_WRITE_YMF262_P1, // 0x5F - YMF262 port 1 write
    VGM_CMD_WRITE_AY8910,    // 0xA0 - AY8910 write
    VGM_CMD_DATA_BLOCK,      // 0x67 - Data block (PCM, etc.)
    VGM_CMD_PCM_RAM_WRITE,   // 0x68 - PCM RAM write
    VGM_CMD_LOOP_START,      // Loop point marker (not a real command)
    VGM_CMD_UNKNOWN          // Unrecognized command
} VgmCommandType;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Chip info - presence and clock rate

typedef struct VgmChipInfo {
    bool present;
    u32 clock;      // Clock rate in Hz (bit 31 may indicate dual chip)
    bool dual_chip; // Two instances of this chip
} VgmChipInfo;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Parsed VGM file

typedef struct VgmFile {
    // Version (BCD format: 0x171 = v1.71)
    u32 version;

    // Timing
    u32 total_samples; // Total playback length in samples
    u32 loop_offset;   // Offset to loop point (0 = no loop)
    u32 loop_samples;  // Number of samples in loop

    // Chip presence and clocks
    VgmChipInfo chips[VGM_CHIP_COUNT];

    // GD3 tag offset (0 = no tag)
    u32 gd3_offset;

    // Command stream
    const u8* data; // Pointer to start of command data
    u64 data_size;  // Size of command data in bytes

    // Original buffer (for reference)
    const u8* buffer;
    u64 buffer_size;
} VgmFile;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Parse result

typedef enum VgmParseStatus {
    VGM_PARSE_OK = 0,
    VGM_PARSE_ERROR_NULL_BUFFER,
    VGM_PARSE_ERROR_TOO_SMALL,
    VGM_PARSE_ERROR_INVALID_MAGIC,
    VGM_PARSE_ERROR_UNSUPPORTED_VERSION,
    VGM_PARSE_ERROR_INVALID_DATA_OFFSET,
    VGM_PARSE_ERROR_ALLOCATION_FAILED
} VgmParseStatus;

typedef struct VgmParseResult {
    VgmFile* file;
    VgmParseStatus status;
} VgmParseResult;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Command structure

typedef struct VgmCommand {
    VgmCommandType type;
    u32 sample_time;  // Absolute sample position when this command occurs
    u8 chip_id;       // For multi-chip setups (0 = first, 1 = second)
    u8 port;          // Port number for multi-port chips
    u8 reg;           // Register address
    u8 value;         // Value to write
    u16 wait_samples; // For VGM_CMD_WAIT: number of samples to wait
} VgmCommand;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Command stream iterator

typedef struct VgmIterator {
    const u8* current;    // Current position in command stream
    const u8* end;        // End of command stream
    const u8* loop_point; // Loop start position (nullptr if no loop)
    u32 sample_position;  // Current time in samples
    bool finished;        // True when end of data reached
} VgmIterator;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// API

// Parse VGM file from buffer
// Returns VgmFile allocated from allocator, or nullptr on error
VgmParseResult vgm_parse(VgmAllocator* alloc, const u8* buffer, u64 size);

// Get human-readable error message for parse status
const char* vgm_parse_status_string(VgmParseStatus status);

// Create iterator for command stream
VgmIterator vgm_iterator_create(const VgmFile* file);

// Get next command from stream
// Returns false when no more commands (finished or error)
bool vgm_iterator_next(VgmIterator* iter, VgmCommand* out_cmd);

// Reset iterator to beginning
void vgm_iterator_reset(VgmIterator* iter, const VgmFile* file);

// Seek iterator to specific sample position
// Iterates through commands until reaching target position
void vgm_iterator_seek(VgmIterator* iter, const VgmFile* file, u32 target_sample);

// Get chip name string
const char* vgm_chip_name(VgmChipId chip_id);

// Get duration in seconds
static inline float vgm_duration_seconds(const VgmFile* file) {
    return (float)file->total_samples / (float)VGM_SAMPLE_RATE;
}

// Check if file has loop point
static inline bool vgm_has_loop(const VgmFile* file) {
    return file->loop_offset > 0 && file->loop_samples > 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Allocator wrappers for use from C++ plugins

// Create allocator for VGM pattern extraction
// Returns nullptr on failure
VgmAllocator* vgm_alloc_create(void);

// Destroy allocator and free all tracked allocations
void vgm_alloc_destroy(VgmAllocator* alloc);

// Free all tracked allocations but keep the allocator alive
void vgm_alloc_rewind(VgmAllocator* alloc);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
