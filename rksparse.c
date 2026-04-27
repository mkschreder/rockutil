/*
 * rksparse.c - Android sparse image expansion used while flashing
 * Rockchip partitions.  Supports v1.0 with chunk types: RAW, FILL,
 * DON'T CARE, and CRC32 (which is just skipped).
 */
#include "rksparse.h"

#include <errno.h>
#include <string.h>

#pragma pack(push, 1)
struct sparse_header {
	uint32_t magic;
	uint16_t major;
	uint16_t minor;
	uint16_t file_hdr_sz;
	uint16_t chunk_hdr_sz;
	uint32_t blk_sz;
	uint32_t total_blks;
	uint32_t total_chunks;
	uint32_t image_checksum;
};

struct chunk_header {
	uint16_t chunk_type;
	uint16_t reserved;
	uint32_t chunk_sz;      /* in blocks */
	uint32_t total_sz;      /* including this header    */
};
#pragma pack(pop)

#define CHUNK_RAW       0xcac1
#define CHUNK_FILL      0xcac2
#define CHUNK_DONT_CARE 0xcac3
#define CHUNK_CRC32     0xcac4

bool rksparse_is_sparse(const void *buf, size_t len)
{
	if (len < sizeof(struct sparse_header))
		return false;
	const struct sparse_header *h = buf;
	return h->magic == RKSPARSE_MAGIC;
}

uint64_t rksparse_total_bytes(const void *buf, size_t len)
{
	if (!rksparse_is_sparse(buf, len))
		return 0;
	const struct sparse_header *h = buf;
	return (uint64_t)h->blk_sz * h->total_blks;
}

int rksparse_expand(const void *buf, size_t len,
                    rksparse_write_fn write_cb, void *user)
{
	if (len < sizeof(struct sparse_header))
		return -EINVAL;
	const struct sparse_header *h = buf;
	if (h->magic != RKSPARSE_MAGIC)
		return -EINVAL;
	if (h->blk_sz % 512 != 0 || h->blk_sz == 0)
		return -EINVAL;

	const uint8_t *cur = (const uint8_t *)buf + h->file_hdr_sz;
	const uint8_t *end = (const uint8_t *)buf + len;
	uint32_t sectors_per_block = h->blk_sz / 512;
	uint32_t cur_lba = 0;

	for (uint32_t i = 0; i < h->total_chunks; ++i) {
		if ((size_t)(end - cur) < h->chunk_hdr_sz)
			return -EINVAL;
		const struct chunk_header *c = (const struct chunk_header *)cur;
		const uint8_t *payload = cur + h->chunk_hdr_sz;
		size_t payload_len = c->total_sz - h->chunk_hdr_sz;
		if ((size_t)(end - cur) < c->total_sz)
			return -EINVAL;

		uint32_t blocks = c->chunk_sz;
		uint32_t sectors = blocks * sectors_per_block;

		switch (c->chunk_type) {
		case CHUNK_RAW:
			if (payload_len != (size_t)blocks * h->blk_sz)
				return -EINVAL;
			if (sectors) {
				int rc = write_cb(user, cur_lba, payload,
				                  sectors, 0);
				if (rc < 0)
					return rc;
			}
			break;
		case CHUNK_FILL: {
			if (payload_len != 4)
				return -EINVAL;
			uint32_t fill;
			memcpy(&fill, payload, 4);
			if (sectors) {
				int rc = write_cb(user, cur_lba, NULL,
				                  sectors, fill);
				if (rc < 0)
					return rc;
			}
			break;
		}
		case CHUNK_DONT_CARE:
			/* skip */
			break;
		case CHUNK_CRC32:
			/* validation only, ignore */
			break;
		default:
			return -EINVAL;
		}

		cur_lba += sectors;
		cur += c->total_sz;
	}
	return 0;
}
