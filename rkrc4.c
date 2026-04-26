/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2024 rockutil contributors
 */
/*
 * rkrc4.c - Rockchip fixed-key RC4 (KSA cycles a 16-byte key).
 */
#include "rkrc4.h"

static const uint8_t rk_rc4_key[16] = {
	0x7c, 0x4e, 0x03, 0x04, 0x55, 0x05, 0x09, 0x07,
	0x2d, 0x2c, 0x7b, 0x38, 0x17, 0x0d, 0x17, 0x11,
};

void rk_rc4(uint8_t *buf, size_t len)
{
	uint8_t s[256];
	for (int i = 0; i < 256; i++)
		s[i] = (uint8_t)i;

	/*
	 * Key schedule: the 16-byte key is reused in a round-robin
	 * fashion but, unlike textbook RC4, the update is
	 *   s[i] = j where j = (j + s[i] + key[i&15]) mod 256 mixed in.
	 * Standard RC4 key schedule with Rockchip's fixed 16-byte key.
	 */
	int j = 0;
	for (int i = 0; i < 256; i++) {
		j = (j + s[i] + rk_rc4_key[i & 15]) & 0xff;
		uint8_t t = s[i];
		s[i] = s[j];
		s[j] = t;
	}

	int i = 0;
	j = 0;
	for (size_t k = 0; k < len; k++) {
		i = (i + 1) & 0xff;
		j = (j + s[i]) & 0xff;
		uint8_t t = s[i];
		s[i] = s[j];
		s[j] = t;
		buf[k] ^= s[(s[i] + s[j]) & 0xff];
	}
}
