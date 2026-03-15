#ifndef __SN76496_H__
#define __SN76496_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "../EmuStructs.h"

extern DEV_DEF devDef_SN76496_MAME;

/* Scope capture API */
void sn76496_set_scope_enabled(void *chip, UINT8 enabled);
UINT8 sn76496_get_scope_enabled(void *chip);
const float* sn76496_get_scope_buffer(void *chip, UINT8 channel, UINT32* out_write_pos);

#ifdef __cplusplus
}
#endif

#endif	// __SN76496_H__
