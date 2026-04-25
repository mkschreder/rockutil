/*
 * rksparse.h - minimal Android sparse-image decoder.  rockutil
 * detects sparse images (magic 0xed26ff3a) in RKAF partitions and
 * expands them while writing to flash so the destination receives the
 * logical image.  Only non-CRC v1.0 sparse is used by Rockchip
 * tooling; that's what this decoder supports.
 */
#ifndef RKSPARSE_H
#define RKSPARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RKSPARSE_MAGIC 0xed26ff3a

/* Tells whether `buf` starts with a sparse header. */
bool rksparse_is_sparse(const void *buf, size_t len);

/*
 * Walks the sparse image and calls `write_cb(user, lba, data, sectors)`
 * for every chunk, producing 512-byte-aligned writes relative to the
 * partition's base LBA.  Returns 0 on success or a negative errno.
 * DON'T CARE blocks are skipped (no callback invocation).
 *
 * If `data` is NULL for a callback invocation, the chunk is a FILL
 * block and `fill_word` holds the repeating 32-bit pattern to expand
 * into `sectors` sectors at LBA `lba`.
 */
typedef int (*rksparse_write_fn)(void *user, uint32_t lba,
                                 const uint8_t *data, uint32_t sectors,
                                 uint32_t fill_word);

int rksparse_expand(const void *buf, size_t len,
                    rksparse_write_fn write_cb, void *user);

#ifdef __cplusplus
}
#endif

#endif
