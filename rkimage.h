/*
 * rkimage.h - Rockchip firmware container parsers (RKFW / RKAF / RKBOOT).
 *
 * Implements parsing of the standard Rockchip on-disk firmware container
 * formats that have been kept stable across RK2918..RK35xx generations
 * and are documented in Rockchip open-source SDK releases.
 *
 * Structure summary
 * -----------------
 *   RKFW (outer firmware package)
 *       0x00  "RKFW"
 *       0x66 byte header holding version/date/chip code,
 *       followed by an embedded RKBOOT loader then an RKAF Android
 *       firmware payload.  A 16-byte MD5 trails the file.
 *
 *   RKBOOT (bootloader package)
 *       0x00  "BOOT" or "LDR "
 *       46-byte header with three entry tables (471 / 472 / loader),
 *       each table carrying variable-length RKBOOT_ENTRY records.  The
 *       last four bytes of the file are a rolled-your-own CRC-32 that
 *       covers everything preceding it.
 *
 *   RKAF (Android firmware archive, similar to a simple cpio)
 *       0x00  "RKAF"
 *       0x66 byte header (model / id / manufacturer strings plus
 *       version + part count) followed by up to 16 partition entries.
 *       Each entry carries a partition name, offset in the archive,
 *       offset on flash (in sectors), and length in bytes.
 */
#ifndef RKIMAGE_H
#define RKIMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* RKBOOT                                                             */
/* ------------------------------------------------------------------ */

enum rkboot_entry_type {
	RKBOOT_ENTRY_471    = 0x01, /* DDR init blob (sent to MaskROM) */
	RKBOOT_ENTRY_472    = 0x02, /* Loader blob (sent to MaskROM)   */
	RKBOOT_ENTRY_LOADER = 0x04, /* Final bootloader (flash write)  */
};

#pragma pack(push, 1)
struct rkboot_header {
	char     magic[4];          /* "BOOT" or "LDR "              */
	uint16_t hdr_size;          /* size of this header           */
	uint32_t version;
	uint32_t merge_version;
	uint16_t year;
	uint8_t  month;
	uint8_t  day;
	uint8_t  hour;
	uint8_t  minute;
	uint8_t  second;
	uint32_t chip;              /* Rockchip chip code            */
	uint8_t  entry471_count;
	uint32_t entry471_offset;
	uint8_t  entry471_size;
	uint8_t  entry472_count;
	uint32_t entry472_offset;
	uint8_t  entry472_size;
	uint8_t  loader_count;
	uint32_t loader_offset;
	uint8_t  loader_size;
	uint8_t  sign_flag;         /* 'S' if signed                 */
	uint8_t  rc4_disable;       /* non-zero => payload is plain  */
};

struct rkboot_entry_raw {
	uint8_t  size;              /* typically 0x39 = 57           */
	uint32_t type;
	uint16_t name[20];          /* UTF-16 LE, null terminated    */
	uint32_t data_offset;
	uint32_t data_size;
	uint32_t data_delay;        /* milliseconds                  */
};
#pragma pack(pop)

struct rkboot_entry {
	enum rkboot_entry_type type;
	char     name[64];          /* UTF-8 conversion of name[]    */
	uint32_t offset;
	uint32_t size;
	uint32_t delay;
};

struct rkboot {
	uint8_t              *raw;        /* mmap'd / malloc'd blob   */
	size_t                raw_len;    /* including CRC tail       */
	struct rkboot_header *hdr;        /* alias into raw           */
	bool                  is_signed;
	bool                  rc4_disabled;
	struct rkboot_entry  *e471;
	size_t                n471;
	struct rkboot_entry  *e472;
	size_t                n472;
	struct rkboot_entry  *loader;
	size_t                nloader;
};

int  rkboot_load_file(struct rkboot *b, const char *path);
int  rkboot_load_buffer(struct rkboot *b, const void *buf, size_t len);
void rkboot_dispose(struct rkboot *b);

/*
 * Copy an entry's payload out of the container.  The returned buffer
 * is malloc'd and must be free()d by the caller.  If the image's
 * rc4_disable flag is clear, the payload is RC4-descrambled using the
 * Rockchip fixed key before being returned (i.e. the returned bytes
 * are always ready to hand to the MaskROM or the Loader).
 */
int rkboot_copy_entry(const struct rkboot *b,
                      const struct rkboot_entry *e,
                      uint8_t **out_buf, size_t *out_len);

/* ------------------------------------------------------------------ */
/* RKFW / RKAF                                                        */
/* ------------------------------------------------------------------ */

#pragma pack(push, 1)
struct rkfw_header {
	char     magic[4];          /* "RKFW"                       */
	uint16_t hdr_size;          /* usually 0x66 = 102           */
	uint8_t  major;             /* FW major version             */
	uint8_t  minor;
	uint8_t  patch;
	uint8_t  build;
	uint32_t code;              /* firmware numeric version     */
	uint16_t year;
	uint8_t  month;
	uint8_t  day;
	uint8_t  hour;
	uint8_t  minute;
	uint8_t  second;
	uint32_t chip;
	uint32_t loader_offset;     /* 0x19                         */
	uint32_t loader_size;
	uint32_t image_offset;      /* 0x21                         */
	uint32_t image_size;
	uint8_t  unknown1;
	uint32_t fs_type;           /* ext4 / vfat hint             */
	uint32_t backup_offset;
	uint32_t backup_size;
	uint8_t  reserved[0x66 - 0x39];
};

/*
 * RKAF partition descriptor (112 bytes, 16-entry array in the
 * header).  Offsets within the archive are byte offsets; nand_size /
 * nand_addr are LBA-sized quantities on the destination flash.
 */
struct rkaf_part {
	char     name[32];
	char     mount[60];
	uint32_t nand_size;         /* 0 => unused slot             */
	uint32_t pos;               /* offset in the archive        */
	uint32_t nand_addr;         /* LBA on flash (0 for param)   */
	uint32_t padded_size;       /* padded LBA count on flash    */
	uint32_t image_size;        /* bytes actually used          */
};

struct rkaf_header {
	char     magic[4];          /* "RKAF"                       */
	uint32_t length;            /* file size - 4 (pre-CRC)      */
	char     model[34];
	char     id[30];
	char     manufacturer[56];
	uint32_t unknown1;
	uint32_t version;
	uint32_t num_parts;
	struct rkaf_part parts[16];
	uint8_t  reserved[0x6D];
};
#pragma pack(pop)

struct rkaf {
	uint8_t           *raw;
	size_t             raw_len;
	struct rkaf_header *hdr;
};

struct rkfw {
	FILE  *fp;
	uint64_t file_size;
	struct rkfw_header hdr;
	/* Loaders embedded in the RKFW package.  For the UL/UF flow
	 * these are what we upload to the MaskROM. */
	struct rkboot boot;
	/* Android firmware payload.  Only populated after a successful
	 * rkfw_load_image(). */
	struct rkaf   image;
};

int  rkfw_open(struct rkfw *f, const char *path);
void rkfw_close(struct rkfw *f);

/* Load the embedded RKBOOT (loader blob).  Sets f->boot. */
int  rkfw_load_boot(struct rkfw *f);

/* Load the embedded RKAF (Android firmware). Sets f->image. */
int  rkfw_load_image(struct rkfw *f);

/*
 * Copy a specific slot from the embedded RKAF into a malloc'd
 * buffer.  part->pos + part->image_size must lie within image->raw_len.
 */
int  rkaf_copy_part(const struct rkaf *img, const struct rkaf_part *p,
                    uint8_t **out, size_t *out_len);

/* Locate a partition by its canonical name ("boot", "system", ...). */
const struct rkaf_part *rkaf_find_part(const struct rkaf *img,
                                       const char *name);

/*
 * Build an RKBOOT container from three raw binary payloads and return
 * a malloc'd blob suitable for writing to disk or uploading via UL.
 *
 * chip        : Rockchip SoC code stored in the header (e.g. 0x33353061).
 * rc4_disable : set non-zero for modern SoCs that carry plain payloads.
 * e471/e472   : DDR-init and USB-plug blobs sent via MaskROM wIndex 0x0471/0x0472.
 * loader      : final bootloader payload.
 *
 * On success returns 0; *out is malloc'd and *out_len is the file size.
 * Caller must free(*out).
 */
int rkboot_build(uint32_t chip, uint8_t rc4_disable,
                 const uint8_t *e471, size_t e471_len,
                 const uint8_t *e472, size_t e472_len,
                 const uint8_t *loader, size_t loader_len,
                 uint8_t **out, size_t *out_len);

/*
 * Extract all entry payloads from a parsed RKBOOT to individual files.
 * Files are named: <output_dir>/471_<i>_<name>.bin  (DDR init)
 *                  <output_dir>/472_<i>_<name>.bin  (USB plug)
 *                  <output_dir>/loader_<i>_<name>.bin (loader)
 * Returns 0 on success.
 */
int rkboot_extract(const struct rkboot *b, const char *output_dir);

/* Pretty-print helpers (used by UF/DI commands). */
void rkboot_print(const struct rkboot *b);
void rkfw_print(const struct rkfw *f);
void rkaf_print(const struct rkaf *img);

#ifdef __cplusplus
}
#endif

#endif /* RKIMAGE_H */
