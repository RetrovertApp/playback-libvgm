# libvgm Playback Plugin
#
# This plugin plays video game music formats using the libvgm library:
# VGM, VGZ (compressed VGM), S98, GYM, and DRO.
#
# Also includes VGM pattern extraction for tracker-style visualization.

cmake_minimum_required(VERSION 3.16)
project(playback_libvgm C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PlaybackPlugin)

# VGM Pattern Extraction library (pure C, no external dependencies)
set(VGM_PATTERN_SOURCES
    src/vgm_alloc.c
    src/vgm_parser.c
    src/vgm_chips.c
    src/vgm_timeline.c
    src/vgm_quantize.c
)

add_library(vgm_pattern STATIC ${VGM_PATTERN_SOURCES})

set_target_properties(vgm_pattern PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(vgm_pattern PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(COMMAND apply_target_config)
    apply_target_config(vgm_pattern)
endif()

if(COMMAND suppress_external_warnings)
    suppress_external_warnings(vgm_pattern)
endif()

if(UNIX)
    target_link_libraries(vgm_pattern PRIVATE m)
endif()

# Find zlib for VGZ decompression
add_subdirectory(external/zlib)

# libvgm emulation core sources (explicitly listed - many files are #included by others)
set(LIBVGM_EMU_SOURCES
    libvgm_src/emu/SoundEmu.c
    libvgm_src/emu/Resampler.c
    libvgm_src/emu/logging.c
    libvgm_src/emu/panning.c
    libvgm_src/emu/dac_control.c
    # SN76496 (DCSG)
    libvgm_src/emu/cores/sn764intf.c
    libvgm_src/emu/cores/sn76496.c
    libvgm_src/emu/cores/sn76489.c
    # YM2413 (OPLL)
    libvgm_src/emu/cores/2413intf.c
    libvgm_src/emu/cores/ym2413.c
    libvgm_src/emu/cores/emu2413.c
    libvgm_src/emu/cores/nukedopll.c
    # YM2612 (OPN2)
    libvgm_src/emu/cores/2612intf.c
    libvgm_src/emu/cores/fmopn.c
    libvgm_src/emu/cores/ym2612.c
    libvgm_src/emu/cores/ym3438.c
    # YM2151 (OPM)
    libvgm_src/emu/cores/2151intf.c
    libvgm_src/emu/cores/ym2151.c
    libvgm_src/emu/cores/nukedopm.c
    # SegaPCM
    libvgm_src/emu/cores/segapcm.c
    # RF5C68
    libvgm_src/emu/cores/rf5cintf.c
    libvgm_src/emu/cores/rf5c68.c
    libvgm_src/emu/cores/scd_pcm.c
    # YM2203/YM2608/YM2610 (OPN family)
    libvgm_src/emu/cores/opnintf.c
    libvgm_src/emu/cores/ymdeltat.c
    # YM3812/YM3526/Y8950 (OPL family)
    libvgm_src/emu/cores/oplintf.c
    libvgm_src/emu/cores/fmopl.c
    libvgm_src/emu/cores/adlibemu_opl2.c
    libvgm_src/emu/cores/adlibemu_opl3.c
    libvgm_src/emu/cores/nukedopl3.c
    # YMF262 (OPL3)
    libvgm_src/emu/cores/262intf.c
    libvgm_src/emu/cores/ymf262.c
    # YMF278B
    libvgm_src/emu/cores/ymf278b.c
    # YMZ280B
    libvgm_src/emu/cores/ymz280b.c
    # YMF271
    libvgm_src/emu/cores/ymf271.c
    # AY8910 (PSG)
    libvgm_src/emu/cores/ayintf.c
    libvgm_src/emu/cores/ay8910.c
    libvgm_src/emu/cores/emu2149.c
    # PWM (32X)
    libvgm_src/emu/cores/pwm.c
    # GameBoy
    libvgm_src/emu/cores/gb.c
    # NES APU
    libvgm_src/emu/cores/nesintf.c
    libvgm_src/emu/cores/nes_apu.c
    libvgm_src/emu/cores/np_nes_apu.c
    libvgm_src/emu/cores/np_nes_dmc.c
    libvgm_src/emu/cores/np_nes_fds.c
    # YMW258 (MultiPCM)
    libvgm_src/emu/cores/multipcm.c
    # uPD7759
    libvgm_src/emu/cores/upd7759.c
    # OKI MSM6258/MSM6295
    libvgm_src/emu/cores/okim6258.c
    libvgm_src/emu/cores/okim6295.c
    libvgm_src/emu/cores/okiadpcm.c
    # Konami K-series
    libvgm_src/emu/cores/k051649.c
    libvgm_src/emu/cores/k054539.c
    libvgm_src/emu/cores/k053260.c
    libvgm_src/emu/cores/k007232.c
    libvgm_src/emu/cores/k005289.c
    # HuC6280
    libvgm_src/emu/cores/c6280intf.c
    libvgm_src/emu/cores/c6280_mame.c
    libvgm_src/emu/cores/Ootake_PSG.c
    # Namco C-series
    libvgm_src/emu/cores/c140.c
    libvgm_src/emu/cores/c219.c
    libvgm_src/emu/cores/c352.c
    # POKEY
    libvgm_src/emu/cores/pokey.c
    # QSound
    libvgm_src/emu/cores/qsoundintf.c
    libvgm_src/emu/cores/qsound_mame.c
    libvgm_src/emu/cores/qsound_ctr.c
    # SCSP
    libvgm_src/emu/cores/scsp.c
    libvgm_src/emu/cores/scspdsp.c
    # WonderSwan
    libvgm_src/emu/cores/ws_audio.c
    # VirtualBoy VSU
    libvgm_src/emu/cores/vsu.c
    # SAA1099
    libvgm_src/emu/cores/saaintf.c
    libvgm_src/emu/cores/saa1099_mame.c
    libvgm_src/emu/cores/saa1099_vb.c
    # Ensoniq
    libvgm_src/emu/cores/es5503.c
    libvgm_src/emu/cores/es5506.c
    # X1-010
    libvgm_src/emu/cores/x1_010.c
    # Irem GA20
    libvgm_src/emu/cores/iremga20.c
    # Mikey
    libvgm_src/emu/cores/mikey.c
    # MSM5205/MSM5232
    libvgm_src/emu/cores/msm5205.c
    libvgm_src/emu/cores/msm5232.c
    # BSMT2000
    libvgm_src/emu/cores/bsmt2000.c
    # ICS2115
    libvgm_src/emu/cores/ics2115.c
)

# libvgm player sources
set(LIBVGM_PLAYER_SOURCES
    libvgm_src/player/playerbase.cpp
    libvgm_src/player/playera.cpp
    libvgm_src/player/vgmplayer.cpp
    libvgm_src/player/vgmplayer_cmdhandler.cpp
    libvgm_src/player/s98player.cpp
    libvgm_src/player/gymplayer.cpp
    libvgm_src/player/droplayer.cpp
    libvgm_src/player/helper.c
    libvgm_src/player/dblk_compr.c
)

# libvgm utils sources
set(LIBVGM_UTILS_SOURCES
    libvgm_src/utils/DataLoader.c
    libvgm_src/utils/MemoryLoader.c
    libvgm_src/utils/FileLoader.c
)

# Platform-specific utils
if(WIN32)
    list(APPEND LIBVGM_UTILS_SOURCES
        libvgm_src/utils/OSThread_Win.c
        libvgm_src/utils/OSMutex_Win.c
        libvgm_src/utils/OSSignal_Win.c
        libvgm_src/utils/StrUtils-CPConv_Win.c
    )
else()
    list(APPEND LIBVGM_UTILS_SOURCES
        libvgm_src/utils/OSThread_POSIX.c
        libvgm_src/utils/OSMutex_POSIX.c
        libvgm_src/utils/OSSignal_POSIX.c
        libvgm_src/utils/StrUtils-CPConv_IConv.c
    )
endif()

add_library(libvgm_playback SHARED
    libvgm_plugin.cpp
    ${LIBVGM_EMU_SOURCES}
    ${LIBVGM_PLAYER_SOURCES}
    ${LIBVGM_UTILS_SOURCES}
)

target_include_directories(libvgm_playback PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libvgm_src
)

target_link_libraries(libvgm_playback PRIVATE
    zlib_full_static
)

target_link_libraries(libvgm_playback PRIVATE vgm_pattern)
target_compile_definitions(libvgm_playback PRIVATE HAS_VGM_PATTERN)

if(NOT WIN32)
    target_link_libraries(libvgm_playback PRIVATE pthread m)
endif()

# iconv is needed for StrUtils-CPConv_IConv.c on non-Windows
if(APPLE)
    target_link_libraries(libvgm_playback PRIVATE iconv)
endif()

# Compile definitions - enable all sound devices
target_compile_definitions(libvgm_playback PRIVATE
    VGM_LITTLE_ENDIAN
    HAVE_STDINT_H
    SNDDEV_SELECT
    SNDDEV_SN76496
    EC_SN76496_MAME
    EC_SN76496_MAXIM
    SNDDEV_YM2413
    EC_YM2413_MAME
    EC_YM2413_EMU2413
    EC_YM2413_NUKED
    SNDDEV_YM2612
    EC_YM2612_GPGX
    EC_YM2612_GENS
    EC_YM2612_NUKED
    SNDDEV_YM2151
    EC_YM2151_MAME
    EC_YM2151_NUKED
    SNDDEV_SEGAPCM
    SNDDEV_RF5C68
    EC_RF5C68_MAME
    EC_RF5C68_GENS
    SNDDEV_YM2203
    SNDDEV_YM2608
    SNDDEV_YM2610
    SNDDEV_YM3812
    EC_YM3812_MAME
    EC_YM3812_ADLIBEMU
    EC_YM3812_NUKED
    SNDDEV_YM3526
    SNDDEV_Y8950
    SNDDEV_YMF262
    EC_YMF262_MAME
    EC_YMF262_ADLIBEMU
    EC_YMF262_NUKED
    SNDDEV_YMF278B
    SNDDEV_YMZ280B
    SNDDEV_YMF271
    SNDDEV_AY8910
    EC_AY8910_MAME
    EC_AY8910_EMU2149
    SNDDEV_32X_PWM
    SNDDEV_GAMEBOY
    SNDDEV_NES_APU
    EC_NES_MAME
    EC_NES_NSFPLAY
    EC_NES_NSFP_FDS
    SNDDEV_YMW258
    SNDDEV_UPD7759
    SNDDEV_MSM6258
    SNDDEV_MSM6295
    SNDDEV_K051649
    SNDDEV_K054539
    SNDDEV_C6280
    EC_C6280_MAME
    EC_C6280_OOTAKE
    SNDDEV_C140
    SNDDEV_C219
    SNDDEV_K053260
    SNDDEV_POKEY
    SNDDEV_QSOUND
    EC_QSOUND_MAME
    EC_QSOUND_CTR
    SNDDEV_SCSP
    SNDDEV_WSWAN
    SNDDEV_VBOY_VSU
    SNDDEV_SAA1099
    EC_SAA1099_MAME
    EC_SAA1099_VB
    SNDDEV_ES5503
    SNDDEV_ES5506
    SNDDEV_X1_010
    SNDDEV_C352
    SNDDEV_GA20
    SNDDEV_MIKEY
    SNDDEV_K007232
    SNDDEV_K005289
    SNDDEV_MSM5205
    SNDDEV_MSM5232
    SNDDEV_BSMT2000
    SNDDEV_ICS2115
)

set_target_properties(libvgm_playback PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
)

rv_set_plugin_properties(libvgm_playback)
