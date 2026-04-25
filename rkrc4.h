/*
 * rkrc4.h - Rockchip variant of RC4.  The update tool uses a fixed
 * 16-byte key cycled through the 256-byte state during KSA.  This is
 * used to encode several sub-containers of RKBOOT (entry payloads)
 * and, when enabled, parts of RKAF firmware.
 */
#ifndef RKRC4_H
#define RKRC4_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* In-place symmetric transform.  Call twice to round-trip. */
void rk_rc4(uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif
