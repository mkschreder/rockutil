/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2024 rockutil contributors
 */
/*
 * rkparam.c - parameter.txt parser + GPT builder.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "rkparam.h"
#include "rkcrc.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* parameter.txt parser                                               */
/* ------------------------------------------------------------------ */

static void str_copy_trim(char *dst, size_t cap, const char *src)
{
	while (*src && isspace((unsigned char)*src))
		++src;

	size_t n = strlen(src);
	while (n && isspace((unsigned char)src[n - 1]))
		--n;

	if (n >= cap)
		n = cap - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static uint64_t parse_u64(const char *s, const char **end_out)
{
	while (*s && isspace((unsigned char)*s))
		++s;

	uint64_t v = 0;
	int base = 10;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	}

	while (*s) {
		int d;
		char c = *s;
		if (c >= '0' && c <= '9') {
			d = c - '0';
		} else if (base == 16 && c >= 'a' && c <= 'f') {
			d = 10 + c - 'a';
		} else if (base == 16 && c >= 'A' && c <= 'F') {
			d = 10 + c - 'A';
		} else {
			break;
		}
		v = v * base + (uint64_t)d;
		++s;
	}

	if (end_out)
		*end_out = s;
	return v;
}

/*
 * Parse a single mtdparts entry: "<size>@<offset>(<name>[,flags])"
 *   size   - hex bytes, or "-" for "to end of device".
 *   offset - hex bytes absolute offset.
 *   name   - partition name between ().
 */
static int parse_mtdparts(const char *cmdline, struct rk_parameter *p)
{
	const char *needle = "mtdparts=";
	const char *mtd = strstr(cmdline, needle);
	if (!mtd)
		return 0;
	mtd += strlen(needle);

	/* skip "<devname>:" */
	const char *colon = strchr(mtd, ':');
	if (!colon)
		return -EINVAL;
	const char *cur = colon + 1;

	while (*cur) {
		while (*cur == ' ' || *cur == '\t' || *cur == ',')
			++cur;
		if (!*cur)
			break;

		uint64_t size;
		if (*cur == '-') {
			size = 0;
			++cur;
		} else {
			const char *after = NULL;
			size = parse_u64(cur, &after) * 512; /* LBA to bytes  */
			cur = after;
		}

		if (*cur != '@')
			return -EINVAL;
		++cur;

		const char *after = NULL;
		uint64_t off = parse_u64(cur, &after) * 512;
		cur = after;

		if (*cur != '(')
			return -EINVAL;
		++cur;
		const char *name_end = strchr(cur, ')');
		if (!name_end)
			return -EINVAL;

		if (p->num_parts >= RKPARAM_MAX_PARTS)
			return -E2BIG;

		struct rk_partition *pp = &p->parts[p->num_parts++];
		size_t name_len = (size_t)(name_end - cur);
		char *comma = memchr(cur, ',', name_len);
		if (comma)
			name_len = (size_t)(comma - cur);
		if (name_len >= sizeof(pp->name))
			name_len = sizeof(pp->name) - 1;
		memcpy(pp->name, cur, name_len);
		pp->name[name_len] = '\0';
		pp->offset = off;
		pp->size = size;
		pp->lba_start = off / 512;
		pp->lba_count = size / 512;

		cur = name_end + 1;
	}
	return 0;
}

int rk_parameter_load_buffer(struct rk_parameter *p, const void *buf,
                             size_t len)
{
	memset(p, 0, sizeof(*p));

	char *data = malloc(len + 1);
	if (!data)
		return -ENOMEM;
	memcpy(data, buf, len);
	data[len] = '\0';

	char *save = NULL;
	char *line = strtok_r(data, "\n", &save);
	while (line) {
		char *colon = strchr(line, ':');
		if (colon) {
			*colon = '\0';
			const char *val = colon + 1;
			if (strcmp(line, "FIRMWARE_VER") == 0)
				str_copy_trim(p->firmware_version,
				              sizeof(p->firmware_version), val);
			else if (strcmp(line, "MACHINE_MODEL") == 0)
				str_copy_trim(p->machine_model,
				              sizeof(p->machine_model), val);
			else if (strcmp(line, "MACHINE_ID") == 0)
				str_copy_trim(p->machine_id,
				              sizeof(p->machine_id), val);
			else if (strcmp(line, "CHECK_MASK") == 0)
				str_copy_trim(p->check_mask,
				              sizeof(p->check_mask), val);
			else if (strcmp(line, "PWR_HLD") == 0)
				str_copy_trim(p->pwr_hold,
				              sizeof(p->pwr_hold), val);
			else if (strcmp(line, "TYPE") == 0)
				str_copy_trim(p->type,
				              sizeof(p->type), val);
			else if (strcmp(line, "CMDLINE") == 0)
				str_copy_trim(p->cmdline,
				              sizeof(p->cmdline), val);
		}
		line = strtok_r(NULL, "\n", &save);
	}

	int rc = parse_mtdparts(p->cmdline, p);
	free(data);
	return rc;
}

int rk_parameter_load_file(struct rk_parameter *p, const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return -errno;
	if (fseek(fp, 0, SEEK_END) != 0) {
		int e = -errno;
		fclose(fp);
		return e;
	}
	long sz = ftell(fp);
	if (sz < 0) {
		int e = -errno;
		fclose(fp);
		return e;
	}
	rewind(fp);

	uint8_t *buf = malloc((size_t)sz);
	if (!buf) {
		fclose(fp);
		return -ENOMEM;
	}
	if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
		free(buf);
		fclose(fp);
		return -EIO;
	}
	fclose(fp);

	int rc = rk_parameter_load_buffer(p, buf, (size_t)sz);
	free(buf);
	return rc;
}

/* ------------------------------------------------------------------ */
/* GPT builder                                                        */
/* ------------------------------------------------------------------ */

#pragma pack(push, 1)
struct gpt_header {
	char     signature[8];    /* "EFI PART"              */
	uint32_t revision;        /* 0x00010000               */
	uint32_t header_size;     /* 92                       */
	uint32_t header_crc32;
	uint32_t reserved;
	uint64_t current_lba;     /* 1                        */
	uint64_t backup_lba;
	uint64_t first_usable_lba;
	uint64_t last_usable_lba;
	uint8_t  disk_guid[16];
	uint64_t part_entry_lba;  /* 2                        */
	uint32_t num_part_entries;/* 128                      */
	uint32_t part_entry_size; /* 128                      */
	uint32_t part_array_crc32;
	uint8_t  padding[512 - 92];
};

struct gpt_entry {
	uint8_t  type_guid[16];
	uint8_t  unique_guid[16];
	uint64_t first_lba;
	uint64_t last_lba;
	uint64_t attributes;
	uint16_t name[36];        /* UTF-16 LE               */
};
#pragma pack(pop)

static void random_guid(uint8_t out[16])
{
	for (int i = 0; i < 16; ++i)
		out[i] = (uint8_t)(rand() & 0xff);
	out[6] = (out[6] & 0x0f) | 0x40; /* version 4 */
	out[8] = (out[8] & 0x3f) | 0x80; /* variant 1 */
}

/*
 * "Basic data partition" GUID: EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
 * stored in mixed-endian form.
 */
static const uint8_t GPT_BASIC_DATA[16] = {
	0xa2, 0xa0, 0xd0, 0xeb,
	0xe5, 0xb9,
	0x33, 0x44,
	0x87, 0xc0,
	0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7,
};

static void utf8_to_utf16le(const char *in, uint16_t *out, size_t out_words)
{
	size_t i = 0;
	while (*in && i + 1 < out_words) {
		uint8_t c = (uint8_t)*in++;
		uint32_t cp;
		if (c < 0x80) {
			cp = c;
		} else if ((c & 0xe0) == 0xc0 && *in) {
			cp = (uint32_t)(c & 0x1f) << 6;
			cp |= (uint8_t)*in++ & 0x3f;
		} else if ((c & 0xf0) == 0xe0 && in[0] && in[1]) {
			cp = (uint32_t)(c & 0x0f) << 12;
			cp |= ((uint8_t)*in++ & 0x3f) << 6;
			cp |= (uint8_t)*in++ & 0x3f;
		} else {
			cp = '?';
		}
		out[i++] = (uint16_t)(cp > 0xffff ? '?' : cp);
	}
	out[i] = 0;
}

int rk_parameter_build_gpt(const struct rk_parameter *p,
                           uint64_t device_lba_count,
                           uint8_t **out, size_t *out_len)
{
	if (device_lba_count < 34 + 33)
		return -EINVAL;

	const size_t total_bytes = 34 * 512; /* primary GPT area */
	uint8_t *blob = calloc(1, total_bytes);
	if (!blob)
		return -ENOMEM;

	/* Protective MBR ------------------------------------------------ */
	blob[510] = 0x55;
	blob[511] = 0xaa;
	uint8_t *mbr_p = &blob[446];
	mbr_p[0] = 0x00;            /* not bootable            */
	mbr_p[1] = 0x00; mbr_p[2] = 0x02; mbr_p[3] = 0x00;
	mbr_p[4] = 0xee;            /* EFI GPT type            */
	mbr_p[5] = 0xff; mbr_p[6] = 0xff; mbr_p[7] = 0xff;
	uint32_t first_lba = 1;
	uint32_t sector_count = (device_lba_count - 1 > 0xffffffffu) ?
	                        0xffffffffu :
	                        (uint32_t)(device_lba_count - 1);
	memcpy(&mbr_p[8],  &first_lba,   4);
	memcpy(&mbr_p[12], &sector_count, 4);

	/* Partition entry array @ LBA 2 -------------------------------- */
	struct gpt_entry *entries =
		(struct gpt_entry *)(blob + 2 * 512);
	for (size_t i = 0; i < p->num_parts && i < 128; ++i) {
		const struct rk_partition *src = &p->parts[i];
		if (src->name[0] == '\0')
			continue;

		struct gpt_entry *e = &entries[i];
		memcpy(e->type_guid, GPT_BASIC_DATA, 16);
		random_guid(e->unique_guid);
		e->first_lba = src->lba_start;
		uint64_t count = src->lba_count;
		if (count == 0) /* "-" -> to end of device */
			count = device_lba_count - 34 - src->lba_start;
		e->last_lba = src->lba_start + count - 1;
		e->attributes = 0;
		utf8_to_utf16le(src->name, e->name, 36);
	}

	/* Primary GPT header @ LBA 1 ----------------------------------- */
	struct gpt_header *hdr = (struct gpt_header *)(blob + 512);
	memcpy(hdr->signature, "EFI PART", 8);
	hdr->revision = 0x00010000;
	hdr->header_size = 92;
	hdr->reserved = 0;
	hdr->current_lba = 1;
	hdr->backup_lba  = device_lba_count - 1;
	hdr->first_usable_lba = 34;
	hdr->last_usable_lba  = device_lba_count - 34;
	random_guid(hdr->disk_guid);
	hdr->part_entry_lba     = 2;
	hdr->num_part_entries   = 128;
	hdr->part_entry_size    = sizeof(struct gpt_entry);
	hdr->part_array_crc32   =
		rkcrc32_le(0, (const uint8_t *)entries,
		           128 * sizeof(struct gpt_entry));
	hdr->header_crc32       = 0;
	hdr->header_crc32       =
		rkcrc32_le(0, (const uint8_t *)hdr, 92);

	*out = blob;
	*out_len = total_bytes;
	return 0;
}
