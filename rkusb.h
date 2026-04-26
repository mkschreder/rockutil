/*
 * rkusb.h - Rockchip Rockusb / Mass-Storage-over-BulkOnly protocol
 *
 * Implements the Rockchip Rockusb protocol over USB Mass Storage Class
 * Bulk-Only Transport (BoT), vendor class 0xFF / subclass 0x06 /
 * protocol 0x05, plus the MaskROM vendor control transfers used to
 * upload the first-stage bootloader.
 *
 * The CBW / CSW layout matches USB MSC BBB.  The CDB carries a
 * Rockchip-specific opcode in byte 0 followed by operation-specific
 * parameters in big-endian form.
 */
#ifndef RKUSB_H
#define RKUSB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libusb-1.0/libusb.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROCKCHIP_VID 0x2207
#define FUZHOU_VID   0x071b
#define BBK_VID      0x0bb4

#define RKUSB_DEFAULT_TIMEOUT_MS 20000u
#define RKUSB_CONTROL_TIMEOUT_MS 5000u
#define RKUSB_MAX_CHUNK          0x1000u /* 4 KiB DRAM/loader chunk   */
#define RKUSB_SECTOR_BYTES       512u
#define RKUSB_MAX_LBA_SECTORS    128u    /* 64 KiB per transfer       */

/* ------------------------------------------------------------------ */
/* device types (matches STRUCT_DEVICE_CONFIG.device_type in the PE)  */
/* ------------------------------------------------------------------ */
enum rkdevice_type {
	RKDEV_NONE       = 0x0000,
	RKDEV_RK27       = 0x0010,
	RKDEV_RK27B      = 0x0011,
	RKDEV_RK28       = 0x0020,
	RKDEV_RKCAYMAN   = 0x0030,
	RKDEV_RK281X     = 0x0021,
	RKDEV_RKNAND     = 0x0022,
	RKDEV_RK26       = 0x0031,
	RKDEV_SMART      = 0x0040,
	RKDEV_RK29       = 0x0050,
	RKDEV_RK292X     = 0x0051,
	RKDEV_RK30       = 0x0060,
	RKDEV_RK30B      = 0x0061,
	RKDEV_RK31       = 0x0070,
	RKDEV_RK32       = 0x0080,
	RKDEV_RK33       = 0x0090,
	RKDEV_RK35       = 0x00a0,
	RKDEV_ALL        = 0xffff,
};

enum rkusb_mode {
	RKUSB_MODE_NONE    = 0,
	RKUSB_MODE_MASKROM = 1,
	RKUSB_MODE_LOADER  = 2,
	RKUSB_MODE_MSC     = 3,
	RKUSB_MODE_UVC     = 4,
};

/* Storage codes for RKOP_CHANGE_STORAGE (CDB[5])                      */
enum rkusb_storage {
	RK_STORAGE_FLASH   = 1,
	RK_STORAGE_EMMC    = 2,
	RK_STORAGE_SD      = 3,
	RK_STORAGE_SD1     = 4,
	RK_STORAGE_SPINOR  = 9,
	RK_STORAGE_SPINAND = 8,
	RK_STORAGE_USB     = 12,
	RK_STORAGE_SATA    = 11,
	RK_STORAGE_PCIE    = 10,
};

/* ------------------------------------------------------------------ */
/* USB operation codes carried in CBW.CBWCB[0]                        */
/* ------------------------------------------------------------------ */
enum rkusb_opcode {
	RKOP_TEST_UNIT_READY = 0x00,
	RKOP_READ_FLASH_ID   = 0x01,
	RKOP_SET_DEVICE_ID   = 0x02,
	RKOP_TEST_BAD_BLOCK  = 0x03,
	RKOP_READ_SECTOR     = 0x04,
	RKOP_WRITE_SECTOR    = 0x05,
	RKOP_ERASE_NORMAL    = 0x06,
	RKOP_ERASE_FORCE     = 0x0b,
	RKOP_READ_LBA        = 0x14,
	RKOP_WRITE_LBA       = 0x15,
	RKOP_ERASE_SYSTEMDISK = 0x16,
	RKOP_READ_SDRAM      = 0x17,
	RKOP_WRITE_SDRAM     = 0x18,
	RKOP_EXECUTE_SDRAM   = 0x19,
	RKOP_READ_FLASH_INFO = 0x1a,
	RKOP_READ_CHIP_INFO  = 0x1b,
	RKOP_SET_RESET_FLAG  = 0x1e,
	RKOP_WRITE_EFUSE     = 0x1f,
	RKOP_READ_EFUSE      = 0x20,
	RKOP_READ_SPARE      = 0x21,
	RKOP_WRITE_SPARE     = 0x22,
	RKOP_ERASE_LBA       = 0x25,
	RKOP_CHANGE_STORAGE  = 0x2b,
	RKOP_READ_CAPABILITY = 0xaa,
	RKOP_DEVICE_RESET    = 0xff,

	/*
	 * Rockchip "loader-mode" commands that aren't part of the
	 * generic Rockusb opcode space.  They are used by the
	 * bootloader after MaskROM hand-off to upload a new IDB/loader,
	 * lower-format the flash, and read vendor storage.
	 */
	RKOP_READ_VENDOR     = 0x56,
	RKOP_WRITE_LOADER    = 0x57,
	RKOP_LOWER_FORMAT    = 0x1d,
	RKOP_ERASE_SECTORS   = 0x29,
};

/* Sub-codes for RKOP_DEVICE_RESET */
enum rkusb_reset_subcode {
	RKRST_NORMAL     = 0x00,
	RKRST_MSC        = 0x01,
	RKRST_POWER_OFF  = 0x02,
	RKRST_MASKROM    = 0x03,
	RKRST_DISCONNECT = 0x04,
};

/* MaskROM control-transfer request codes (wIndex) */
#define RKUSB_CTRL_REQ_DDR_INIT  0x0471
#define RKUSB_CTRL_REQ_LOADER    0x0472

/* ------------------------------------------------------------------ */
/* Bulk-only MSC structures                                           */
/* ------------------------------------------------------------------ */
#define CBW_SIGNATURE 0x43425355u  /* "USBC" little-endian */
#define CSW_SIGNATURE 0x53425355u  /* "USBS" little-endian */

#pragma pack(push, 1)
struct cbw {
	uint32_t signature;   /* CBW_SIGNATURE                            */
	uint32_t tag;         /* random, echoed back in matching CSW      */
	uint32_t data_length; /* bytes to transfer in data stage          */
	uint8_t  flags;       /* 0x80 = IN, 0x00 = OUT                    */
	uint8_t  lun;         /* 0                                        */
	uint8_t  cdb_length;  /* 6 or 10                                  */
	uint8_t  cdb[16];     /* CDB[0] = rkusb_opcode, remainder is BE   */
};

struct csw {
	uint32_t signature;
	uint32_t tag;
	uint32_t residue;
	uint8_t  status;      /* 0 = passed, 1 = failed, 2 = phase error  */
};
#pragma pack(pop)

/* ------------------------------------------------------------------ */
/* Discovered Rockusb device description                              */
/* ------------------------------------------------------------------ */
struct rkdev_desc {
	libusb_device        *dev;             /* reference-counted       */
	uint16_t              vid;
	uint16_t              pid;
	uint16_t              bcdUSB;          /* USB spec number; bit 0=1 → Loader */
	uint16_t              device_type;     /* rkdevice_type           */
	uint16_t              usb_type;        /* rkusb_mode              */
	uint8_t               iManufacturer;   /* USB string descriptor index */
	uint8_t               iProduct;        /* USB string descriptor index */
	uint8_t               bus;
	uint8_t               port_path[7];
	int                   port_path_len;
	uint32_t              location_id;
	char                  serial[64];
};

/* Runtime handle on an opened Rockusb device */
struct rkusb {
	libusb_device_handle *handle;
	struct rkdev_desc     desc;
	int                   interface;
	uint8_t               ep_in;
	uint8_t               ep_out;
	bool                  cbw_parity; /* splits CBW and data stage    */
	unsigned              timeout_ms;
};

/* ------------------------------------------------------------------ */
/* Library lifecycle                                                  */
/* ------------------------------------------------------------------ */
int  rkusb_library_init(void);
void rkusb_library_exit(void);

/* ------------------------------------------------------------------ */
/* Enumeration                                                        */
/* ------------------------------------------------------------------ */
struct rkdev_list {
	struct rkdev_desc *devs;
	size_t             count;
};

int  rkusb_enumerate(struct rkdev_list *out);
void rkusb_free_list(struct rkdev_list *list);

const char *rkusb_mode_name(uint16_t mode);
const char *rkusb_device_type_name(uint16_t dev_type);

/* ------------------------------------------------------------------ */
/* Open / close a single device                                       */
/* ------------------------------------------------------------------ */
int  rkusb_open(struct rkusb *u, const struct rkdev_desc *desc);

/*
 * Like rkusb_open(), but discovers the interface and endpoint layout by
 * issuing live GET_DESCRIPTOR control transfers rather than reading from
 * libusb's potentially stale internal cache.  Use this when the device
 * has performed an in-place USB re-enumeration (same bus/device address)
 * without a physical disconnect — libusb ignores the "change" udev event
 * so its cached descriptors still reflect the old firmware.
 *
 * Returns 0 on success, -ENODEV if the live descriptor shows the device
 * is still in MaskROM mode (iManufacturer=0, iProduct=0), or another
 * negative libusb error code on failure.
 */
int  rkusb_open_live(struct rkusb *u, const struct rkdev_desc *desc);
void rkusb_close(struct rkusb *u);
int  rkusb_reset_pipe(struct rkusb *u, uint8_t pipe);

/* ------------------------------------------------------------------ */
/* High-level commands (all return 0 on success, negative errno-ish)  */
/* ------------------------------------------------------------------ */
int rkusb_test_unit_ready(struct rkusb *u);
int rkusb_reset_device(struct rkusb *u, uint8_t subcode);
int rkusb_read_flash_id(struct rkusb *u, uint8_t id[5]);
int rkusb_read_flash_info(struct rkusb *u, uint8_t info[11]);
int rkusb_read_chip_info(struct rkusb *u, uint8_t info[16]);
int rkusb_read_capability(struct rkusb *u, uint8_t cap[8]);

int rkusb_read_lba(struct rkusb *u, uint32_t lba, uint16_t sectors,
                   uint8_t *buf);
int rkusb_write_lba(struct rkusb *u, uint32_t lba, uint16_t sectors,
                    const uint8_t *buf);
int rkusb_erase_lba(struct rkusb *u, uint32_t lba, uint32_t sectors);

int rkusb_write_sdram(struct rkusb *u, uint32_t addr, const uint8_t *buf,
                      uint32_t len);
int rkusb_execute_sdram(struct rkusb *u, uint32_t addr);

/* Loader-mode specific commands */
int rkusb_switch_storage(struct rkusb *u, uint8_t storage);
int rkusb_lower_format(struct rkusb *u);
int rkusb_erase_blocks(struct rkusb *u, uint32_t lba, uint16_t sectors);
int rkusb_write_loader_cmd(struct rkusb *u, const uint8_t *buf, uint32_t len);

/* Generic MSC-style CBW runner for ad-hoc opcodes. direction: 0=OUT, 1=IN. */
int rkusb_raw_cmd(struct rkusb *u, uint8_t opcode, int direction,
                  const uint8_t *cdb_extra, size_t cdb_extra_len,
                  uint8_t *data, uint32_t data_len);

/*
 * MaskROM control-transfer loader upload.
 * `req_code` is either RKUSB_CTRL_REQ_DDR_INIT or RKUSB_CTRL_REQ_LOADER.
 * The CRC-CCITT tail is appended by this function; pass the raw loader
 * payload.
 */
int rkusb_ctrl_download(struct rkusb *u, uint16_t req_code,
                        const uint8_t *payload, size_t payload_len);

/*
 * Probe the device mode.  Many newer Rockchip chips ship PIDs that
 * aren't in our static table, so we can't rely on enumeration alone.
 * This sends TestUnitReady + ReadFlashInfo in Loader-supported form:
 *   - both fail         => MaskROM (needs handshake)
 *   - at least TUR OK   => LOADER  (writes supported)
 *
 * The probe resets any stall it may have triggered so callers can
 * issue the actual transfer immediately afterward.
 */
int rkusb_probe_loader(struct rkusb *u);

#ifdef __cplusplus
}
#endif

#endif /* RKUSB_H */
