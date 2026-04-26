/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2024 rockutil contributors
 */
/*
 * rkcrc.h - CRC helpers used by Rockchip firmware/MaskROM protocols.
 *
 * Three variants show up in the Rockchip stack:
 *   1. CRC-CCITT (Kermit):  poly 0x1021, init 0x0000 - MaskROM loader tail.
 *   2. Rockchip RKFW CRC32: poly 0x04c10db7, init 0, no reflection - stored
 *      at the end of the RKBOOT container.
 *   3. Standard IEEE 802.3 CRC32-LE: poly 0xedb88320 reflected - used in the
 *      GPT partition-table trailer.
 */
#ifndef RKCRC_H
#define RKCRC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint16_t rkcrc_ccitt(const uint8_t *data, size_t len);
uint32_t rkcrc_rkfw(const uint8_t *data, size_t len);
uint32_t rkcrc32_le(uint32_t seed, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
