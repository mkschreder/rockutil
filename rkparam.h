/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2024 rockutil contributors
 */
/*
 * rkparam.h - parser for Rockchip parameter.txt files and a tiny GPT
 *             builder that converts the partition table into a proper
 *             GUID Partition Table suitable for `DI gpt dump.bin` style
 *             workflows.
 *
 * Format of parameter.txt (see also Rockchip's docs/rk_docs):
 *     FIRMWARE_VER:1.0.0
 *     MACHINE_MODEL:MyBoard
 *     MACHINE_ID:007
 *     CMDLINE:mtdparts=rk29xxnand:0x00002000@0x00002000(uboot),...
 *
 * The CMDLINE mtdparts entry is the authoritative list of partitions:
 *     <size>@<offset>(<name>[,<flags>])
 *   with "-" meaning "to end of device".
 */
#ifndef RKPARAM_H
#define RKPARAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RKPARAM_MAX_PARTS 64

struct rk_partition {
	char     name[36];
	uint64_t offset;    /* bytes within the target device      */
	uint64_t size;      /* bytes; 0 means "rest of device"     */
	uint64_t lba_start; /* offset / 512                         */
	uint64_t lba_count; /* size / 512                           */
};

struct rk_parameter {
	char    firmware_version[64];
	char    machine_model[64];
	char    machine_id[64];
	char    check_mask[64];
	char    pwr_hold[64];
	char    type[64];
	char    cmdline[4096];
	struct rk_partition parts[RKPARAM_MAX_PARTS];
	size_t  num_parts;
};

int rk_parameter_load_file(struct rk_parameter *p, const char *path);
int rk_parameter_load_buffer(struct rk_parameter *p, const void *buf,
                             size_t len);

/*
 * Build a GPT in RAM spanning `device_lba_count` sectors and return
 * the full primary GPT (protective MBR + GPT header + partition
 * entries, up to LBA 34).  `*out` is malloc'd; `*out_len` is set to
 * exactly 34 * 512 = 17408 bytes.
 */
int rk_parameter_build_gpt(const struct rk_parameter *p,
                           uint64_t device_lba_count,
                           uint8_t **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
