/*
 * scope_capture.h - Standardized per-channel scope capture for chip emulators
 *
 * This module provides a common implementation for capturing per-channel audio
 * data during chip emulation, used for oscilloscope/waveform visualization.
 *
 * Usage:
 *   1. Add ScopeCapture to your chip state struct
 *   2. Call scope_init() during chip initialization
 *   3. Call scope_write() in your update loop for each channel
 *   4. Implement thin wrapper API functions that delegate to scope_* helpers
 */

#ifndef SCOPE_CAPTURE_H
#define SCOPE_CAPTURE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "../stdtype.h"
#include "../common_def.h"  /* for INLINE */
#include <string.h>  /* for memset */

#define SCOPE_BUFFER_SIZE 1024
#define SCOPE_BUFFER_MASK (SCOPE_BUFFER_SIZE - 1)
#define SCOPE_MAX_CHANNELS 8

/* Embeddable scope capture state - add this to your chip struct */
typedef struct ScopeCapture {
	UINT8 enabled;
	UINT8 channel_count;
	UINT32 write_pos[SCOPE_MAX_CHANNELS];
	float buffer[SCOPE_MAX_CHANNELS][SCOPE_BUFFER_SIZE];
} ScopeCapture;

/*
 * Initialize scope capture state
 * Call this during chip initialization
 *
 * sc: pointer to ScopeCapture embedded in chip struct
 * channel_count: number of channels this chip has (max SCOPE_MAX_CHANNELS)
 */
INLINE void scope_init(ScopeCapture* sc, UINT8 channel_count)
{
	UINT8 i;
	sc->enabled = 0;
	sc->channel_count = (channel_count > SCOPE_MAX_CHANNELS) ? SCOPE_MAX_CHANNELS : channel_count;
	for (i = 0; i < sc->channel_count; i++)
		sc->write_pos[i] = 0;
}

/*
 * Write a sample to the scope buffer
 * Call this in your update loop for each channel's output
 *
 * sc: pointer to ScopeCapture
 * channel: channel index (0 to channel_count-1)
 * sample: raw audio sample value (INT32)
 * scale: divisor for normalization (output will be sample/scale, clamped to [-1, 1])
 *
 * Typical scale values:
 *   - PSG chips: 2000-6000
 *   - FM chips: 3000-4000
 */
INLINE void scope_write(ScopeCapture* sc, UINT8 channel, INT32 sample, float scale)
{
	UINT32 pos;
	float val;

	if (!sc->enabled || channel >= sc->channel_count)
		return;

	pos = sc->write_pos[channel];
	val = (float)sample / scale;

	if (val > 1.0f)
		val = 1.0f;
	else if (val < -1.0f)
		val = -1.0f;

	sc->buffer[channel][pos] = val;
	sc->write_pos[channel] = (pos + 1) & SCOPE_BUFFER_MASK;
}

/*
 * Enable or disable scope capture
 * When enabling, buffers are cleared
 *
 * sc: pointer to ScopeCapture
 * enabled: 1 to enable, 0 to disable
 */
INLINE void scope_set_enabled(ScopeCapture* sc, UINT8 enabled)
{
	UINT8 i;

	sc->enabled = enabled;
	if (enabled)
	{
		for (i = 0; i < sc->channel_count; i++)
		{
			sc->write_pos[i] = 0;
			memset(sc->buffer[i], 0, sizeof(sc->buffer[i]));
		}
	}
}

/*
 * Check if scope capture is enabled
 */
INLINE UINT8 scope_get_enabled(const ScopeCapture* sc)
{
	return sc->enabled;
}

/*
 * Get the scope buffer for a channel
 *
 * sc: pointer to ScopeCapture
 * channel: channel index
 * out_write_pos: if not NULL, receives current write position
 *
 * Returns: pointer to float buffer of SCOPE_BUFFER_SIZE samples, or NULL if invalid channel
 */
INLINE const float* scope_get_buffer(const ScopeCapture* sc, UINT8 channel, UINT32* out_write_pos)
{
	if (channel >= sc->channel_count)
		return NULL;
	if (out_write_pos != NULL)
		*out_write_pos = sc->write_pos[channel];
	return sc->buffer[channel];
}

#ifdef __cplusplus
}
#endif

#endif /* SCOPE_CAPTURE_H */
