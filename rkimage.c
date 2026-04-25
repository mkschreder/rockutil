/*
 * rkimage.c - parsers for RKFW / RKAF / RKBOOT containers.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "rkimage.h"
#include "rkcrc.h"
#include "rkrc4.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* Small utilities                                                    */
/* ------------------------------------------------------------------ */

static void utf16le_to_utf8(const uint16_t *src, size_t src_words,
                            char *dst, size_t dst_cap)
{
	size_t di = 0;
	for (size_t i = 0; i < src_words && di + 1 < dst_cap; ++i) {
		uint16_t c = src[i];
		if (!c)
			break;
		if (c < 0x80) {
			dst[di++] = (char)c;
		} else if (c < 0x800) {
			if (di + 2 >= dst_cap)
				break;
			dst[di++] = (char)(0xc0 | (c >> 6));
			dst[di++] = (char)(0x80 | (c & 0x3f));
		} else {
			if (di + 3 >= dst_cap)
				break;
			dst[di++] = (char)(0xe0 | (c >> 12));
			dst[di++] = (char)(0x80 | ((c >> 6) & 0x3f));
			dst[di++] = (char)(0x80 | (c & 0x3f));
		}
	}
	if (di < dst_cap)
		dst[di] = '\0';
	else
		dst[dst_cap - 1] = '\0';
}

static int slurp_file(const char *path, uint8_t **out, size_t *out_len)
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
		int e = -EIO;
		free(buf);
		fclose(fp);
		return e;
	}
	fclose(fp);

	*out = buf;
	*out_len = (size_t)sz;
	return 0;
}

/* ------------------------------------------------------------------ */
/* RKBOOT                                                             */
/* ------------------------------------------------------------------ */

static int rkboot_decode_table(const uint8_t *raw, size_t raw_len,
                               uint32_t table_off, uint8_t count,
                               uint8_t entry_size,
                               struct rkboot_entry **out,
                               size_t *out_count)
{
	*out = NULL;
	*out_count = 0;
	if (!count)
		return 0;
	if (entry_size < sizeof(struct rkboot_entry_raw))
		return -EINVAL;
	if ((size_t)table_off + (size_t)count * entry_size > raw_len)
		return -EINVAL;

	struct rkboot_entry *arr = calloc(count, sizeof(*arr));
	if (!arr)
		return -ENOMEM;

	for (uint8_t i = 0; i < count; ++i) {
		const struct rkboot_entry_raw *re =
			(const struct rkboot_entry_raw *)(raw + table_off +
			                                   i * entry_size);
		if ((size_t)re->data_offset + (size_t)re->data_size > raw_len) {
			free(arr);
			return -EINVAL;
		}
		arr[i].type   = (enum rkboot_entry_type)re->type;
		arr[i].offset = re->data_offset;
		arr[i].size   = re->data_size;
		arr[i].delay  = re->data_delay;
		utf16le_to_utf8(re->name, 20, arr[i].name, sizeof(arr[i].name));
	}

	*out = arr;
	*out_count = count;
	return 0;
}

int rkboot_load_buffer(struct rkboot *b, const void *buf, size_t len)
{
	memset(b, 0, sizeof(*b));
	if (len < sizeof(struct rkboot_header) + 4)
		return -EINVAL;

	uint8_t *raw = malloc(len);
	if (!raw)
		return -ENOMEM;
	memcpy(raw, buf, len);

	const struct rkboot_header *hdr = (const struct rkboot_header *)raw;
	if (memcmp(hdr->magic, "BOOT", 4) != 0 &&
	    memcmp(hdr->magic, "LDR ", 4) != 0) {
		free(raw);
		return -EINVAL;
	}

	/* Validate the trailing CRC-32 computed over everything but the
	 * last four bytes.  Rockchip stores it little-endian. */
	uint32_t stored = (uint32_t)raw[len - 4] |
	                  ((uint32_t)raw[len - 3] << 8) |
	                  ((uint32_t)raw[len - 2] << 16) |
	                  ((uint32_t)raw[len - 1] << 24);
	uint32_t computed = rkcrc_rkfw(raw, len - 4);
	if (stored != computed) {
		/* Some legacy RKBOOT blobs ship without the CRC tail
		 * (notably the ones embedded in RKFW packages that
		 * validate via MD5).  Warn but continue. */
		fprintf(stderr,
		        "warning: RKBOOT CRC mismatch (stored=0x%08x "
		        "computed=0x%08x) - assuming embedded blob\n",
		        stored, computed);
	}

	b->raw = raw;
	b->raw_len = len;
	b->hdr = (struct rkboot_header *)raw;
	b->is_signed = hdr->sign_flag == 'S';
	b->rc4_disabled = hdr->rc4_disable != 0;

	int rc;
	rc = rkboot_decode_table(raw, len, hdr->entry471_offset,
	                         hdr->entry471_count, hdr->entry471_size,
	                         &b->e471, &b->n471);
	if (rc < 0) goto fail;
	rc = rkboot_decode_table(raw, len, hdr->entry472_offset,
	                         hdr->entry472_count, hdr->entry472_size,
	                         &b->e472, &b->n472);
	if (rc < 0) goto fail;
	rc = rkboot_decode_table(raw, len, hdr->loader_offset,
	                         hdr->loader_count, hdr->loader_size,
	                         &b->loader, &b->nloader);
	if (rc < 0) goto fail;

	return 0;

fail:
	rkboot_dispose(b);
	return rc;
}

int rkboot_load_file(struct rkboot *b, const char *path)
{
	uint8_t *raw = NULL;
	size_t len = 0;
	int rc = slurp_file(path, &raw, &len);
	if (rc < 0)
		return rc;

	rc = rkboot_load_buffer(b, raw, len);
	free(raw);
	return rc;
}

void rkboot_dispose(struct rkboot *b)
{
	if (!b)
		return;
	free(b->e471);
	free(b->e472);
	free(b->loader);
	free(b->raw);
	memset(b, 0, sizeof(*b));
}

int rkboot_copy_entry(const struct rkboot *b, const struct rkboot_entry *e,
                      uint8_t **out_buf, size_t *out_len)
{
	if ((size_t)e->offset + e->size > b->raw_len)
		return -EINVAL;

	uint8_t *buf = malloc(e->size);
	if (!buf)
		return -ENOMEM;
	memcpy(buf, b->raw + e->offset, e->size);

	if (!b->rc4_disabled)
		rk_rc4(buf, e->size);

	*out_buf = buf;
	*out_len = e->size;
	return 0;
}

static void dump_entry_group(const struct rkboot *b,
                             const char *label,
                             const struct rkboot_entry *e, size_t n)
{
	for (size_t i = 0; i < n; ++i) {
		const struct rkboot_entry *en = &e[i];
		printf("  %s[%zu]: type=0x%02x off=0x%08x size=%-7u "
		       "delay=%u ms name=\"%s\"\n",
		       label, i, en->type, en->offset, en->size,
		       en->delay, en->name);
		size_t head_n = en->size < 16 ? en->size : 16;
		size_t tail_off = en->size > 16 ? en->size - 16 : 0;
		size_t tail_n = en->size - tail_off;
		printf("    head:");
		for (size_t k = 0; k < head_n; ++k)
			printf(" %02x", b->raw[en->offset + k]);
		printf("\n    tail:");
		for (size_t k = 0; k < tail_n; ++k)
			printf(" %02x", b->raw[en->offset + tail_off + k]);
		printf("\n");
	}
}

void rkboot_print(const struct rkboot *b)
{
	const struct rkboot_header *h = b->hdr;
	printf("RKBOOT header:\n");
	printf("  version        : %u.%u.%u.%u\n",
	       (h->version >> 24) & 0xff, (h->version >> 16) & 0xff,
	       (h->version >> 8) & 0xff,  h->version & 0xff);
	printf("  chip code      : 0x%08x\n", h->chip);
	printf("  signed         : %s\n", b->is_signed ? "yes" : "no");
	printf("  rc4 disabled   : %s\n", b->rc4_disabled ? "yes" : "no");
	printf("  hdr_size       : %u\n", h->hdr_size);
	printf("  e471 tbl_off   : 0x%08x count=%u rec_size=%u\n",
	       h->entry471_offset, h->entry471_count, h->entry471_size);
	printf("  e472 tbl_off   : 0x%08x count=%u rec_size=%u\n",
	       h->entry472_offset, h->entry472_count, h->entry472_size);
	printf("  loader tbl_off : 0x%08x count=%u rec_size=%u\n",
	       h->loader_offset, h->loader_count, h->loader_size);
	printf("  raw_len        : %zu\n", b->raw_len);
	printf("  entries 471    : %zu\n", b->n471);
	dump_entry_group(b, "471",    b->e471,   b->n471);
	printf("  entries 472    : %zu\n", b->n472);
	dump_entry_group(b, "472",    b->e472,   b->n472);
	printf("  entries loader : %zu\n", b->nloader);
	dump_entry_group(b, "loader", b->loader, b->nloader);
}

/* ------------------------------------------------------------------ */
/* RKFW                                                               */
/* ------------------------------------------------------------------ */

int rkfw_open(struct rkfw *f, const char *path)
{
	memset(f, 0, sizeof(*f));
	f->fp = fopen(path, "rb");
	if (!f->fp)
		return -errno;

	struct stat st;
	if (stat(path, &st) != 0) {
		int e = -errno;
		fclose(f->fp);
		f->fp = NULL;
		return e;
	}
	f->file_size = (uint64_t)st.st_size;

	if (fread(&f->hdr, 1, sizeof(f->hdr), f->fp) != sizeof(f->hdr)) {
		int e = -EIO;
		fclose(f->fp);
		f->fp = NULL;
		return e;
	}

	if (memcmp(f->hdr.magic, "RKFW", 4) != 0) {
		fclose(f->fp);
		f->fp = NULL;
		return -EINVAL;
	}

	if ((uint64_t)f->hdr.loader_offset + f->hdr.loader_size > f->file_size ||
	    (uint64_t)f->hdr.image_offset + f->hdr.image_size > f->file_size) {
		fclose(f->fp);
		f->fp = NULL;
		return -EINVAL;
	}
	return 0;
}

void rkfw_close(struct rkfw *f)
{
	if (!f)
		return;
	rkboot_dispose(&f->boot);
	free(f->image.raw);
	memset(&f->image, 0, sizeof(f->image));
	if (f->fp)
		fclose(f->fp);
	f->fp = NULL;
}

int rkfw_load_boot(struct rkfw *f)
{
	uint8_t *buf = malloc(f->hdr.loader_size);
	if (!buf)
		return -ENOMEM;
	if (fseek(f->fp, f->hdr.loader_offset, SEEK_SET) != 0) {
		free(buf);
		return -errno;
	}
	if (fread(buf, 1, f->hdr.loader_size, f->fp) != f->hdr.loader_size) {
		free(buf);
		return -EIO;
	}

	int rc = rkboot_load_buffer(&f->boot, buf, f->hdr.loader_size);
	free(buf);
	return rc;
}

int rkfw_load_image(struct rkfw *f)
{
	if (f->image.raw)
		return 0;

	uint8_t *buf = malloc(f->hdr.image_size);
	if (!buf)
		return -ENOMEM;
	if (fseek(f->fp, f->hdr.image_offset, SEEK_SET) != 0) {
		free(buf);
		return -errno;
	}
	if (fread(buf, 1, f->hdr.image_size, f->fp) != f->hdr.image_size) {
		free(buf);
		return -EIO;
	}

	if (memcmp(buf, "RKAF", 4) != 0) {
		free(buf);
		return -EINVAL;
	}

	f->image.raw = buf;
	f->image.raw_len = f->hdr.image_size;
	f->image.hdr = (struct rkaf_header *)buf;
	return 0;
}

void rkfw_print(const struct rkfw *f)
{
	printf("RKFW header:\n");
	printf("  version     : %u.%u.%u.%u (code 0x%08x)\n",
	       f->hdr.major, f->hdr.minor, f->hdr.patch, f->hdr.build,
	       f->hdr.code);
	printf("  date        : %04u-%02u-%02u %02u:%02u:%02u\n",
	       f->hdr.year, f->hdr.month, f->hdr.day,
	       f->hdr.hour, f->hdr.minute, f->hdr.second);
	printf("  chip code   : 0x%08x\n", f->hdr.chip);
	printf("  loader      : offset=0x%08x size=0x%08x\n",
	       f->hdr.loader_offset, f->hdr.loader_size);
	printf("  image       : offset=0x%08x size=0x%08x\n",
	       f->hdr.image_offset, f->hdr.image_size);
}

/* ------------------------------------------------------------------ */
/* RKAF                                                               */
/* ------------------------------------------------------------------ */

const struct rkaf_part *rkaf_find_part(const struct rkaf *img,
                                       const char *name)
{
	if (!img || !img->hdr)
		return NULL;
	for (uint32_t i = 0;
	     i < img->hdr->num_parts && i < 16;
	     ++i) {
		const struct rkaf_part *p = &img->hdr->parts[i];
		if (strncmp(p->name, name, sizeof(p->name)) == 0)
			return p;
	}
	return NULL;
}

int rkaf_copy_part(const struct rkaf *img, const struct rkaf_part *p,
                   uint8_t **out, size_t *out_len)
{
	if (!img || !p)
		return -EINVAL;
	if ((size_t)p->pos + p->image_size > img->raw_len)
		return -EINVAL;

	uint8_t *buf = malloc(p->image_size ? p->image_size : 1);
	if (!buf)
		return -ENOMEM;
	memcpy(buf, img->raw + p->pos, p->image_size);
	*out = buf;
	*out_len = p->image_size;
	return 0;
}

void rkaf_print(const struct rkaf *img)
{
	const struct rkaf_header *h = img->hdr;
	printf("RKAF header:\n");
	printf("  model       : %.34s\n",  h->model);
	printf("  id          : %.30s\n",  h->id);
	printf("  manufacturer: %.56s\n",  h->manufacturer);
	printf("  version     : 0x%08x\n", h->version);
	printf("  partitions  : %u\n",     h->num_parts);
	for (uint32_t i = 0; i < h->num_parts && i < 16; ++i) {
		const struct rkaf_part *p = &h->parts[i];
		printf("   [%2u] %-20.32s pos=0x%08x size=0x%08x "
		       "flash@LBA=0x%08x pad=0x%08x\n",
		       i, p->name, p->pos, p->image_size,
		       p->nand_addr, p->padded_size);
	}
}
