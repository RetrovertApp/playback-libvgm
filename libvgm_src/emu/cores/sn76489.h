#ifndef __SN76489_H__
#define __SN76489_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "../EmuStructs.h"

extern DEV_DEF devDef_SN76489_Maxim;

/* Scope capture API */
void sn76489_set_scope_enabled(void *chip, UINT8 enabled);
UINT8 sn76489_get_scope_enabled(void *chip);
const float* sn76489_get_scope_buffer(void *chip, UINT8 channel, UINT32* out_write_pos);

#ifdef __cplusplus
}
#endif

#endif	// __SN76489_H__
