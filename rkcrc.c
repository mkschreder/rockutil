/*
 * rkcrc.c - CRC variants used by Rockchip update tooling.
 */
#include "rkcrc.h"

uint16_t rkcrc_ccitt(const uint8_t *data, size_t len)
{
	/*
	 * CRC-CCITT/FALSE: poly=0x1021, init=0xFFFF, non-reflected, no
	 * final XOR.  The CRC_CCITT() algorithm initialises the accumulator
	 * to 0xFFFFFFFF, making the effective 16-bit init
	 * 0xFFFF.  A wrong init value (e.g. 0x0000 / CRC-XMODEM) produces
	 * a different checksum that the MaskROM silently accepts for the DDR
	 * init blob (471) but then refuses for the loader blob (472),
	 * causing EP0 to NAK indefinitely (LIBUSB_ERROR_TIMEOUT).
	 */
	uint16_t crc = 0xFFFF;
	for (size_t i = 0; i < len; i++) {
		crc ^= (uint16_t)data[i] << 8;
		for (int b = 0; b < 8; b++) {
			if (crc & 0x8000)
				crc = (uint16_t)((crc << 1) ^ 0x1021);
			else
				crc <<= 1;
		}
	}
	return crc;
}

/*
 * Rockchip RKFW CRC32 with polynomial 0x04c10db7 (non-reflected, no XOR
 * at the end).  The boot container stores this value little-endian at
 * the last four bytes of the file.
 */
uint32_t rkcrc_rkfw(const uint8_t *data, size_t len)
{
	uint32_t crc = 0;
	for (size_t i = 0; i < len; i++) {
		crc ^= ((uint32_t)data[i]) << 24;
		for (int b = 0; b < 8; b++) {
			if (crc & 0x80000000u)
				crc = (crc << 1) ^ 0x04c10db7u;
			else
				crc <<= 1;
		}
	}
	return crc;
}

/*
 * Standard zlib / GPT CRC-32: reflected poly 0xedb88320, init 0xffffffff,
 * final xor 0xffffffff.  Used by GPT headers and partition entry arrays.
 */
uint32_t rkcrc32_le(uint32_t seed, const uint8_t *data, size_t len)
{
	uint32_t crc = seed ^ 0xffffffffu;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++) {
			uint32_t mask = -(int32_t)(crc & 1u);
			crc = (crc >> 1) ^ (0xedb88320u & mask);
		}
	}
	return crc ^ 0xffffffffu;
}
