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

/* UBI image magic (big-endian: 'U','B','I','#' = 0x55424923) */
#define RKUBI_MAGIC 0x23494255u

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

/*
 * Build both primary and backup GPT regions.
 * `*out` is malloc'd and contains two sections:
 *   [0 .. 34*512)             - primary GPT area (protective MBR + header + entries)
 *   [34*512 .. 34*512+33*512) - backup GPT area (entries + backup header)
 * `*out_len` = (34 + 33) * 512 = 34304 bytes.
 * Callers are responsible for writing the primary section to LBA 0
 * and the backup section to LBA (device_lba_count - 33).
 */
int rk_parameter_build_gpt_full(const struct rk_parameter *p,
                                uint64_t device_lba_count,
                                uint8_t **out, size_t *out_len);

/*
 * Parse a GPT blob (e.g. read from LBA 0 on the device) and find the
 * partition named `name`.  The name is compared case-sensitively.
 * On success returns 0 and sets *first_lba_out / *last_lba_out.
 * Returns -ENOENT if the partition is not found.
 * `len` must be at least 34 * 512 bytes.
 */
int rk_gpt_find_part(const uint8_t *gpt_blob, size_t len,
                     const char *name,
                     uint64_t *first_lba_out, uint64_t *last_lba_out);

/*
 * Pretty-print the partition table from a GPT blob to stdout.
 * Skips empty entries (zero type GUID).
 */
void rk_gpt_print(const uint8_t *gpt_blob, size_t len);

#ifdef __cplusplus
}
#endif

#endif
