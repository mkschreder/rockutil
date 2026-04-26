/* Enable POSIX/XOPEN APIs (fseeko, ftello, off_t, strcasecmp). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/*
 * rockutil.c - CLI driver.
 *
 * rockutil — CLI driver for Rockchip device flashing and management
 * over the Rockusb/MaskROM protocol.
 *
 * Supported commands:
 *   H | -h | --help               show usage
 *   V | -v | --version            show version
 *   LD                            list attached Rockusb devices
 *   TD                            test unit ready
 *   RD [subcode]                  reset device
 *   RP [pipe]                     reset bulk pipe
 *   RID                           read flash ID
 *   RFI                           read flash info
 *   RCI                           read chip info
 *   RCB                           read capability bitmap
 *   RL  <begin> <count> [file]    read LBA sectors (to stdout or file)
 *   WL  <begin> <file> [--size N] write LBA sectors from a file
 *   EL  <begin> <count>           erase LBA sectors
 *   DB  <loader.bin> [--code N]   send raw loader/ddr init via MaskROM
 *                                 control transfer (code=0x471|0x472)
 *   UL  <loader>                  upgrade loader (MaskROM -> Loader)
 *   UF  <firmware.img>            upgrade full firmware package (RKFW)
 *   DI  <part> <file> [param.txt] download a partition image (raw or sparse)
 *   GPT <parameter.txt> <outfile> build a GPT from parameter.txt
 *   EF  <parameter.txt>           erase flash (per-partition)
 *   PRINT <firmware.img>          dump RKFW / RKBOOT / RKAF header info
 *   SS  <storage>                 switch storage (emmc|nand|sd|spinor|spinand)
 */
#include <errno.h>
#include <inttypes.h>
#include <libusb-1.0/libusb.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "rkimage.h"
#include "rkparam.h"
#include "rksparse.h"
#include "rkusb.h"

#define TOOL_NAME     "rockutil"
#define TOOL_VERSION  "1.0"

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */
static int parse_ulong(const char *s, unsigned long *out)
{
	if (!s || !*s)
		return -1;
	char *end = NULL;
	int base = 10;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		base = 16;
	errno = 0;
	unsigned long v = strtoul(s, &end, base);
	if (errno || !end || *end != '\0')
		return -1;
	*out = v;
	return 0;
}

static void print_hex(const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; ++i)
		printf("%02X ", buf[i]);
	printf("\n");
}

static void print_usage(void)
{
	fputs(
	"---------------------Tool Usage ---------------------\n"
	"Help:             H | -h | --help\n"
	"Version:          V | -v | --version\n"
	"ListDevice:       LD\n"
	"TestDevice:       TD\n"
	"ResetDevice:      RD  [subcode]\n"
	"ResetPipe:        RP  [0|1]            (0 = OUT, 1 = IN)\n"
	"ReadFlashID:      RID\n"
	"ReadFlashInfo:    RFI\n"
	"ReadChipInfo:     RCI\n"
	"ReadCapability:   RCB\n"
	"ReadLBA:          RL  <begin> <count> [outfile]\n"
	"WriteLBA:         WL  <begin> <file>  [--size sectors]\n"
	"EraseLBA:         EL  <begin> <count>\n"
	"DownloadBoot:     DB  <loader.bin>    [--code 0x471|0x472]\n"
	"UpgradeLoader:    UL  <loader.bin|firmware.img>\n"
	"UpgradeFirmware:  UF  <firmware.img>\n"
	"DownloadImage:    DI  <PartName> <image>  [parameter.txt]\n"
	"CreateGPT:        GPT <parameter.txt> <outfile>\n"
	"EraseFlash:       EF  <parameter.txt>\n"
	"PrintImage:       PRINT <firmware.img>\n"
	"SwitchStorage:    SS  <emmc|nand|sd|spinor|spinand>\n"
	"-------------------------------------------------------\n",
	stdout);
}

/* ------------------------------------------------------------------ */
/* Device selection                                                   */
/* ------------------------------------------------------------------ */
static int scan_and_select(struct rkdev_list *list, int *selected_index)
{
	int rc = rkusb_enumerate(list);
	if (rc < 0) {
		fprintf(stderr, "enumerate failed: %s\n", libusb_error_name(rc));
		return rc;
	}
	if (list->count == 0) {
		fprintf(stderr, "No Rockusb device found.\n");
		return -ENODEV;
	}
	/*
	 * The original tool supported per-port selection via the CD
	 * (ChooseDevice) command.  When exactly one device is attached
	 * we pick it implicitly; otherwise the caller must run LD, note
	 * the DevNo and set ROCKUSB_DEV to it.
	 */
	const char *env = getenv("ROCKUSB_DEV");
	int want = env ? atoi(env) - 1 : 0;
	if (want < 0 || (size_t)want >= list->count) {
		fprintf(stderr,
		        "Multiple (%zu) devices attached; set ROCKUSB_DEV=N.\n",
		        list->count);
		rkusb_free_list(list);
		return -ERANGE;
	}
	*selected_index = want;
	return 0;
}

static int open_selected(struct rkusb *u)
{
	struct rkdev_list list = {0};
	int idx = 0;
	int rc = scan_and_select(&list, &idx);
	if (rc < 0)
		return rc;
	rc = rkusb_open(u, &list.devs[idx]);
	if (rc < 0)
		fprintf(stderr, "rkusb_open failed: %s\n",
		        libusb_error_name(rc));
	rkusb_free_list(&list);
	return rc;
}

/* ------------------------------------------------------------------ */
/* Command handlers                                                    */
/* ------------------------------------------------------------------ */
static int cmd_list(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	struct rkdev_list list = {0};
	int rc = rkusb_enumerate(&list);
	if (rc < 0) {
		fprintf(stderr, "enumerate failed: %s\n",
		        libusb_error_name(rc));
		return 1;
	}
	printf("List of rockusb connected (%zu)\n", list.count);
	if (list.count == 0) {
		printf("No found any rockusb device, please plug device in!\n");
	}
	for (size_t i = 0; i < list.count; ++i) {
		const struct rkdev_desc *d = &list.devs[i];
		printf("DevNo=%zu\tVid=0x%04X,Pid=0x%04X,LocationID=%x\t"
		       "Mode=%s\tSerialNo=%s\n",
		       i + 1, d->vid, d->pid, d->location_id,
		       rkusb_mode_name(d->usb_type), d->serial);
	}
	rkusb_free_list(&list);
	return 0;
}

static int cmd_test(void)
{
	struct rkusb u;
	int rc = open_selected(&u);
	if (rc < 0)
		return 1;
	rc = rkusb_test_unit_ready(&u);
	if (rc == 0)
		printf("Test Device OK.\n");
	else
		fprintf(stderr, "Test Device failed (%d)\n", rc);
	rkusb_close(&u);
	return rc ? 1 : 0;
}

static int cmd_reset(int argc, char **argv)
{
	unsigned long sub = 0;
	if (argc >= 1 && parse_ulong(argv[0], &sub) < 0) {
		fprintf(stderr, "Parameter of [RD] command is invalid.\n");
		return 1;
	}
	struct rkusb u;
	if (open_selected(&u) < 0)
		return 1;
	int rc = rkusb_reset_device(&u, (uint8_t)sub);
	if (rc == 0)
		printf("Reset Device OK.\n");
	else
		fprintf(stderr, "Reset Device failed (%d)\n", rc);
	rkusb_close(&u);
	return rc ? 1 : 0;
}

static int cmd_reset_pipe(int argc, char **argv)
{
	unsigned long pipe = 0;
	if (argc >= 1 && parse_ulong(argv[0], &pipe) < 0) {
		fprintf(stderr, "Parameter of [RP] command is invalid.\n");
		return 1;
	}
	struct rkusb u;
	if (open_selected(&u) < 0)
		return 1;
	int rc = rkusb_reset_pipe(&u, (uint8_t)pipe);
	if (rc == 0)
		printf("Reset Pipe OK.\n");
	else
		fprintf(stderr, "Reset Pipe failed (%d)\n", rc);
	rkusb_close(&u);
	return rc ? 1 : 0;
}

static int cmd_read_flash_id(void)
{
	struct rkusb u;
	if (open_selected(&u) < 0)
		return 1;
	uint8_t id[5];
	int rc = rkusb_read_flash_id(&u, id);
	if (rc == 0) {
		printf("Flash ID: ");
		print_hex(id, sizeof(id));
	} else {
		fprintf(stderr, "Read flash ID failed (%d)\n", rc);
	}
	rkusb_close(&u);
	return rc ? 1 : 0;
}

static int cmd_read_flash_info(void)
{
	struct rkusb u;
	if (open_selected(&u) < 0)
		return 1;
	uint8_t info[11];
	int rc = rkusb_read_flash_info(&u, info);
	if (rc == 0) {
		uint32_t flash_size  = (uint32_t)info[0]
		                     | ((uint32_t)info[1] << 8)
		                     | ((uint32_t)info[2] << 16)
		                     | ((uint32_t)info[3] << 24);
		uint16_t block_size  = (uint16_t)info[4]
		                     | ((uint16_t)info[5] << 8);
		uint8_t  page_size   = info[6];
		uint16_t ecc_bits    = (uint16_t)info[7]
		                     | ((uint16_t)info[8] << 8);
		uint8_t  access_time = info[9];
		uint8_t  manufacturer= info[10];
		printf("Flash Info:\n");
		printf("\tFlash Size: %u sectors (%.2f GiB)\n",
		       flash_size,
		       flash_size / (double)(2 * 1024 * 1024));
		printf("\tBlock Size: %u sectors\n", block_size);
		printf("\tPage Size:  %u sectors\n", page_size);
		printf("\tECC Bits:   %u\n", ecc_bits);
		printf("\tAccess time:%u\n", access_time);
		printf("\tManufacturer code: 0x%02X\n", manufacturer);
	} else {
		fprintf(stderr, "Read flash info failed (%d)\n", rc);
	}
	rkusb_close(&u);
	return rc ? 1 : 0;
}

static int cmd_read_chip_info(void)
{
	struct rkusb u;
	if (open_selected(&u) < 0)
		return 1;
	uint8_t info[16];
	int rc = rkusb_read_chip_info(&u, info);
	if (rc == 0) {
		/*
		 * The ChipInfo string is endian-swapped inside each
		 * 32-bit word by the Rockchip ROM.  Reverse it so the
		 * user sees e.g. "RK3399" directly.
		 */
		char text[17] = {0};
		for (int i = 0; i < 16; i += 4) {
			text[i + 0] = (char)info[i + 3];
			text[i + 1] = (char)info[i + 2];
			text[i + 2] = (char)info[i + 1];
			text[i + 3] = (char)info[i + 0];
		}
		printf("Chip Info: ");
		print_hex(info, sizeof(info));
		printf("Chip:      %s\n", text);
	} else {
		fprintf(stderr, "Read chip info failed (%d)\n", rc);
	}
	rkusb_close(&u);
	return rc ? 1 : 0;
}

static int cmd_read_capability(void)
{
	struct rkusb u;
	if (open_selected(&u) < 0)
		return 1;
	uint8_t cap[8];
	int rc = rkusb_read_capability(&u, cap);
	if (rc == 0) {
		printf("Capability: ");
		print_hex(cap, sizeof(cap));
		static const char *FEATURES[] = {
			"Direct LBA", "Vendor Storage", "First 4m Access",
			"Read LBA", "New Efuse", "Read COM Log",
			"Read IDB", "Read SN",
		};
		for (int i = 0; i < 8; ++i)
			if (cap[0] & (1u << i))
				printf("  - %s: enabled\n", FEATURES[i]);
	} else {
		fprintf(stderr, "Read capability failed (%d)\n", rc);
	}
	rkusb_close(&u);
	return rc ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* RL / WL / EL                                                       */
/* ------------------------------------------------------------------ */
static int cmd_read_lba(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "Parameter of [RL] command is invalid.\n");
		return 1;
	}
	unsigned long begin, count;
	if (parse_ulong(argv[0], &begin) || parse_ulong(argv[1], &count)) {
		fprintf(stderr, "BeginSec or SectorLen is invalid.\n");
		return 1;
	}
	const char *outpath = argc >= 3 ? argv[2] : NULL;
	FILE *out = stdout;
	if (outpath) {
		out = fopen(outpath, "wb");
		if (!out) {
			fprintf(stderr, "Read LBA failed, can't open %s: %s\n",
			        outpath, strerror(errno));
			return 1;
		}
	}

	struct rkusb u;
	if (open_selected(&u) < 0) {
		if (out != stdout)
			fclose(out);
		return 1;
	}

	uint8_t chunk[RKUSB_MAX_LBA_SECTORS * RKUSB_SECTOR_BYTES];
	unsigned long remaining = count;
	unsigned long lba       = begin;
	int rc = 0;
	while (remaining) {
		uint16_t step = remaining > RKUSB_MAX_LBA_SECTORS
		                ? (uint16_t)RKUSB_MAX_LBA_SECTORS
		                : (uint16_t)remaining;
		rc = rkusb_read_lba(&u, (uint32_t)lba, step, chunk);
		if (rc < 0) {
			fprintf(stderr,
			        "ReadLBA failed at sector %lu: %d\n", lba, rc);
			break;
		}
		if (fwrite(chunk, RKUSB_SECTOR_BYTES, step, out) != step) {
			fprintf(stderr, "fwrite failed: %s\n", strerror(errno));
			rc = -EIO;
			break;
		}
		lba       += step;
		remaining -= step;
	}

	rkusb_close(&u);
	if (out != stdout)
		fclose(out);
	return rc ? 1 : 0;
}

static int cmd_write_lba(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "Parameter of [WL] command is invalid.\n");
		return 1;
	}
	unsigned long begin;
	if (parse_ulong(argv[0], &begin)) {
		fprintf(stderr, "BeginSec is invalid.\n");
		return 1;
	}
	const char *path = argv[1];
	unsigned long forced_sectors = 0;
	for (int i = 2; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--size") &&
		    parse_ulong(argv[i + 1], &forced_sectors) == 0)
			continue;
	}

	FILE *in = fopen(path, "rb");
	if (!in) {
		fprintf(stderr, "Write LBA failed, can't open %s: %s\n",
		        path, strerror(errno));
		return 1;
	}
	fseeko(in, 0, SEEK_END);
	off_t size = ftello(in);
	fseeko(in, 0, SEEK_SET);

	struct rkusb u;
	if (open_selected(&u) < 0) {
		fclose(in);
		return 1;
	}

	uint8_t chunk[RKUSB_MAX_LBA_SECTORS * RKUSB_SECTOR_BYTES];
	unsigned long total_sectors =
		forced_sectors ? forced_sectors
		               : (unsigned long)((size + RKUSB_SECTOR_BYTES - 1)
		                                 / RKUSB_SECTOR_BYTES);
	unsigned long lba = begin;
	int rc = 0;
	while (total_sectors) {
		uint16_t step = total_sectors > RKUSB_MAX_LBA_SECTORS
		                ? (uint16_t)RKUSB_MAX_LBA_SECTORS
		                : (uint16_t)total_sectors;
		size_t want = (size_t)step * RKUSB_SECTOR_BYTES;
		memset(chunk, 0, want);
		size_t got = fread(chunk, 1, want, in);
		(void)got; /* short read: remainder is zero-padded */
		rc = rkusb_write_lba(&u, (uint32_t)lba, step, chunk);
		if (rc < 0) {
			fprintf(stderr, "WriteLBA failed at sector %lu: %d\n",
			        lba, rc);
			break;
		}
		lba           += step;
		total_sectors -= step;
	}

	rkusb_close(&u);
	fclose(in);
	return rc ? 1 : 0;
}

static int cmd_erase_lba(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "Parameter of [EL] command is invalid.\n");
		return 1;
	}
	unsigned long begin, count;
	if (parse_ulong(argv[0], &begin) || parse_ulong(argv[1], &count)) {
		fprintf(stderr, "BeginSec or EraseCount is invalid.\n");
		return 1;
	}

	struct rkusb u;
	if (open_selected(&u) < 0)
		return 1;
	printf("Start to erase lba begin(0x%lx) count(0x%lx)...\n",
	       begin, count);
	int rc = rkusb_erase_lba(&u, (uint32_t)begin, (uint32_t)count);
	if (rc == 0)
		printf("Erase LBA OK.\n");
	else
		fprintf(stderr, "Erase LBA failed (%d)\n", rc);
	rkusb_close(&u);
	return rc ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Download Boot (MaskROM)                                            */
/* ------------------------------------------------------------------ */
static int cmd_download_boot(int argc, char **argv)
{
	if (argc < 1) {
		fprintf(stderr, "Parameter of [DB] command is invalid.\n");
		return 1;
	}
	const char *path = argv[0];
	unsigned long code = RKUSB_CTRL_REQ_LOADER;
	for (int i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--code") &&
		    parse_ulong(argv[i + 1], &code) == 0)
			continue;
	}

	FILE *in = fopen(path, "rb");
	if (!in) {
		fprintf(stderr,
		        "Open loader failed (%d), exit download boot!\n",
		        errno);
		return 1;
	}
	fseeko(in, 0, SEEK_END);
	off_t size = ftello(in);
	fseeko(in, 0, SEEK_SET);
	if (size <= 0) {
		fclose(in);
		fprintf(stderr, "Loader is empty.\n");
		return 1;
	}
	uint8_t *buf = malloc((size_t)size);
	if (!buf) {
		fclose(in);
		fprintf(stderr, "oom\n");
		return 1;
	}
	if (fread(buf, 1, (size_t)size, in) != (size_t)size) {
		fprintf(stderr, "short read on %s\n", path);
		free(buf);
		fclose(in);
		return 1;
	}
	fclose(in);

	struct rkusb u;
	int rc = open_selected(&u);
	if (rc < 0) {
		free(buf);
		return 1;
	}

	printf("Download Boot Start (code=0x%lx, %ld bytes)\n", code, (long)size);
	rc = rkusb_ctrl_download(&u, (uint16_t)code, buf, (size_t)size);
	rkusb_close(&u);
	free(buf);
	if (rc == 0) {
		printf("Download Boot Success\n");
		return 0;
	}
	fprintf(stderr, "Download Boot Fail (%d)\n", rc);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Progress                                                           */
/* ------------------------------------------------------------------ */
static void progress(const char *label, uint64_t cur, uint64_t total)
{
	static int last = -1;
	int pct = total ? (int)((cur * 100ull) / total) : 100;
	if (pct == last)
		return;
	last = pct;
	fprintf(stderr, "\r%s ... %3d%%", label, pct);
	if (cur == total) {
		fputc('\n', stderr);
		last = -1;
	}
	fflush(stderr);
}

/* ------------------------------------------------------------------ */
/* Loader (DDR init + loader) upload sequence                         */
/* ------------------------------------------------------------------ */
static int upload_boot_entries(struct rkusb *u, const struct rkboot *b,
                               const struct rkboot_entry *tbl, size_t n,
                               uint16_t req_code, const char *label)
{
	for (size_t i = 0; i < n; ++i) {
		uint8_t *payload = NULL;
		size_t   plen    = 0;
		int rc = rkboot_copy_entry(b, &tbl[i], &payload, &plen);
		if (rc < 0) {
			fprintf(stderr, "%s[%zu] copy failed: %d\n",
			        label, i, rc);
			return rc;
		}
		rc = rkusb_ctrl_download(u, req_code, payload, plen);
		free(payload);
		if (rc < 0) {
			fprintf(stderr, "%s[%zu] upload failed: %d\n",
			        label, i, rc);
			return rc;
		}
		if (tbl[i].delay)
			usleep((unsigned)tbl[i].delay * 1000u);
		progress(label, i + 1, n);
	}
	if (n == 0)
		progress(label, 1, 1);
	return 0;
}

/*
 * Core MaskROM handshake: send every 0x471 entry then every 0x472 entry.
 *
 * Send all 0x471 (DDR-init) entries then all 0x472 (usbplug) entries on
 * the same libusb handle with no close/reopen between the stages.
 * The USB device reset happens after the 0x472 binary executes.
 *
 * The MaskROM also validates a CRC-CCITT/FALSE (poly=0x1021, init=0xFFFF)
 * appended to each blob.  The original CRC_CCITT() function initialises its
 * accumulator to 0xFFFFFFFF, making the effective 16-bit init 0xFFFF.
 * Using init=0x0000 (CRC-XMODEM) produces a different checksum that the
 * MaskROM silently accepts for 471 blobs but then refuses for 472, causing
 * EP0 to NAK indefinitely (LIBUSB_ERROR_TIMEOUT).  rkcrc_ccitt() has been
 * corrected accordingly.
 */
static int maskrom_handshake(struct rkusb *u, const struct rkboot *boot)
{
	int rc = upload_boot_entries(u, boot, boot->e471, boot->n471,
	                             RKUSB_CTRL_REQ_DDR_INIT, "DDR init");
	if (rc < 0)
		return rc;

	if (boot->n472 == 0)
		return 0;

	/*
	 * Send 472 on the same handle -- the MaskROM's USB controller stays
	 * up throughout both stages.  The entry's data_delay field (applied
	 * inside upload_boot_entries after each successful send) provides any
	 * inter-stage pause the firmware requires.
	 */
	rc = upload_boot_entries(u, boot, boot->e472, boot->n472,
	                         RKUSB_CTRL_REQ_LOADER, "Loader   ");
	return rc;
}

/*
 * After the 472 (usbplug) upload completes the MaskROM jumps to the
 * usbplug entry point.  usbplug re-initialises the USB controller and
 * re-enumerates as a LOADER-mode device (e.g. PID 0x110D for RV1106).
 *
 * The MaskROM handle must be closed BEFORE polling.  While it is open,
 * libusb holds a reference to the underlying libusb_device for the
 * MaskROM PID (0x110C).  libusb_get_device_list returns referenced
 * devices even after the physical device disconnects, so the stale
 * 0x110C entry appears first in the enumeration results and masks the
 * new 0x110D (Loader) device.  Closing the handle drops that reference
 * so the device list reflects only what is physically present.
 *
 * The polling loop waits for a LOADER-mode device (usb_type ==
 * RKUSB_MODE_LOADER) and skips any MASKROM devices that may transiently
 * reappear.  Three consecutive stable detections are sufficient; fewer
 * would risk opening the device before its BOT endpoint is ready, more
 * would risk missing the usbplug window before its watchdog fires.
 *
 * Polling budget: 120 × 50 ms = 6 seconds.
 */
static int wait_for_loader_mode(struct rkusb *u)
{
	/*
	 * Step 1: drop the MaskROM handle.
	 */
	rkusb_close(u);

	/*
	 * Step 2: reinitialise the libusb context.
	 *
	 * On Linux, libusb with udev hotplug support serves
	 * libusb_get_device_list() from its internal device cache
	 * (ctx->usb_devs).  The cache is populated once at libusb_init()
	 * via linux_scan_devices() and updated only by udev "add"/"remove"
	 * hotplug events processed by the background thread.
	 *
	 * The RV1106 transitions from MaskROM to usbplug via an IN-PLACE
	 * USB bus reset — no physical disconnect, same bus/device address.
	 * The kernel re-reads the device and config descriptors (iMfr
	 * changes from 0 to 1, bulk endpoints appear) but the udev event
	 * type is "change"/"bind", which libusb silently ignores.  The
	 * stale MaskROM cache entry stays in ctx->usb_devs PERMANENTLY;
	 * libusb_get_device_descriptor() always returns iMfr=0 regardless
	 * of how many times we poll or reinitialise the context.
	 *
	 * A libusb reinit does help for SoCs that DO physically disconnect
	 * (new device address → new session_id → fresh cache entry on the
	 * initial linux_scan_devices() call if usbplug is already up).
	 * Keep it for those devices.
	 */
	rkusb_library_exit();
	if (rkusb_library_init() < 0) {
		fprintf(stderr, "libusb reinit failed — cannot poll for Loader.\n");
		return -ENODEV;
	}

	/*
	 * Step 3: poll for up to 120 s (2400 × 50 ms).
	 *
	 * The RV1106 usbplug firmware physically disconnects from USB at
	 * ~T+5 s (after the MaskROM loader upload completes) so it can
	 * initialise DDR and NAND/eMMC flash.  The device then reconnects
	 * ~25–35 s later in Loader mode with a fresh device address.
	 *
	 * Two detection paths run in parallel every poll iteration:
	 *
	 *   Path A — standard enumeration: device appears as LOADER in
	 *     libusb's cache (fresh "add" udev event → correct iMfr=1 →
	 *     LOADER classification via the tiebreaker).  This is the
	 *     normal path for the RV1106 after its physical reconnect.
	 *
	 *   Path B — sysfs probe: for SoCs that perform an in-place USB
	 *     bus reset (no physical disconnect), libusb's cache stays
	 *     stale.  Reads /sys/bus/usb/devices/BUS-PATH/descriptors —
	 *     zero USB traffic, non-intrusive — and detects iMfr→1.
	 */

	uint16_t prev_vid  = 0;
	uint16_t prev_pid  = 0;
	uint16_t prev_type = 0xFFFF;
	bool     announced_disconnect = false;

	for (int tries = 0; tries < 2400; ++tries) {
		usleep(50 * 1000);

		struct rkdev_list list = {0};
		rkusb_enumerate(&list);

		bool              found_loader  = false;
		bool              found_maskrom = false;
		struct rkdev_desc loader_copy   = {0};
		struct rkdev_desc maskrom_copy  = {0};
		libusb_device    *loader_dev    = NULL;
		libusb_device    *maskrom_dev   = NULL;

		for (size_t i = 0; i < list.count; ++i) {
			const struct rkdev_desc *d = &list.devs[i];

			if (d->usb_type != RKUSB_MODE_MASKROM ||
			    prev_type == RKUSB_MODE_LOADER) {
				if (d->vid != prev_vid || d->pid != prev_pid ||
				    d->usb_type != prev_type) {
					fprintf(stderr,
					        "  [T+%.1fs] VID=0x%04X PID=0x%04X"
					        " mode=%s\n",
					        (tries + 1) * 0.05,
					        d->vid, d->pid,
					        rkusb_mode_name(d->usb_type));
				}
			}
			prev_vid  = d->vid;
			prev_pid  = d->pid;
			prev_type = d->usb_type;

			if (!found_loader && d->usb_type == RKUSB_MODE_LOADER) {
				found_loader = true;
				loader_copy  = *d;
				loader_dev   = libusb_ref_device(d->dev);
			}
			if (!found_maskrom && d->usb_type == RKUSB_MODE_MASKROM) {
				found_maskrom = true;
				maskrom_copy  = *d;
				maskrom_dev   = libusb_ref_device(d->dev);
			}
		}

		if (list.count == 0 && prev_type != 0xFFFF) {
			if (!announced_disconnect) {
				fprintf(stderr,
				        "  USB disconnect — usbplug initialising flash,"
				        " waiting up to 120 s...\n");
				announced_disconnect = true;
			}
			prev_vid  = 0;
			prev_pid  = 0;
			prev_type = 0xFFFF;
		}

		/* Print a dot every 5 s while no device is visible. */
		if (list.count == 0 && tries > 0 && tries % 100 == 0)
			fprintf(stderr, "  [T+%.0fs] still waiting...\n",
			        (tries + 1) * 0.05);

		rkusb_free_list(&list);

		/* Path A: standard open for LOADER-classified devices. */
		if (found_loader) {
			loader_copy.dev = loader_dev;
			int opened = rkusb_open(u, &loader_copy);
			libusb_unref_device(loader_dev);
			loader_dev = NULL;

			if (opened == 0) {
				if (found_maskrom && maskrom_dev)
					libusb_unref_device(maskrom_dev);
				fprintf(stderr,
				        "Loader mode ready "
				        "(VID=0x%04X PID=0x%04X"
				        " ep_in=0x%02X ep_out=0x%02X iface=%d)\n",
				        u->desc.vid, u->desc.pid,
				        u->ep_in, u->ep_out, u->interface);
				return 0;
			}
			fprintf(stderr,
			        "  [open] VID=0x%04X PID=0x%04X failed: %s"
			        " — will retry\n",
			        loader_copy.vid, loader_copy.pid,
			        libusb_error_name(opened));
			prev_type = 0xFFFF;
		}

		/*
		 * Path B: sysfs-descriptor probe for MASKROM-classified devices.
		 * Reads /sys/bus/usb/devices/BUS-PATH/descriptors — no USB fd
		 * open, no USB traffic.  Detects in-place re-enumeration where
		 * the device transitions to usbplug without physically
		 * disconnecting (libusb's cache remains stale indefinitely).
		 */
		if (found_maskrom) {
			maskrom_copy.dev = maskrom_dev;
			int opened = rkusb_open_live(u, &maskrom_copy);
			libusb_unref_device(maskrom_dev);
			maskrom_dev = NULL;

			if (opened == 0) {
				fprintf(stderr,
				        "Loader mode ready via sysfs probe "
				        "(VID=0x%04X PID=0x%04X"
				        " ep_in=0x%02X ep_out=0x%02X iface=%d)\n",
				        u->desc.vid, u->desc.pid,
				        u->ep_in, u->ep_out, u->interface);
				return 0;
			}
		/*
		 * -ENODEV  = still MaskROM or sysfs unavailable → keep polling
		 * other    = transient open/claim error → keep polling
		 */
			(void)opened;
		}

		/*
		 * After the physical USB disconnect, the device re-enumerates
		 * as a Loader once usbplug has finished initialising flash
		 * (~25–50 s later).  The libusb udev background thread should
		 * process the "add" event, but empirically it sometimes does
		 * not — the reconnected device never appears in the cache.
		 *
		 * A fresh libusb_init() always calls linux_scan_devices() which
		 * reads the current USB state directly from sysfs and is
		 * guaranteed to see the reconnected device.  Reinitialise the
		 * libusb context every second after the disconnect so we never
		 * miss the reconnect by more than 1 s.
		 */
		if (announced_disconnect && !found_loader && !found_maskrom &&
		    tries % 20 == 0) {
			rkusb_library_exit();
			if (rkusb_library_init() < 0) {
				fprintf(stderr, "libusb reinit failed\n");
				return -ENODEV;
			}
		}
	}
	fprintf(stderr, "Timed out waiting for Loader mode (120 s).\n");
	return -ETIMEDOUT;
}

/*
 * Single entry point used by UL/UF/DI/EF: open, probe, and if the
 * device is actually in MaskROM, run the handshake and wait for the
 * loader to show up.  Caller must dispose of `boot_for_handshake`.
 */
static int open_and_ensure_loader(struct rkusb *u,
                                  const struct rkboot *boot_for_handshake)
{
	if (open_selected(u) < 0)
		return -ENODEV;

	/*
	 * Trust the known-device table first.  MaskROM devices do NOT
	 * implement Bulk-Only Transport, and sending them a CBW (as
	 * rkusb_probe_loader does) wedges the vendor-class control
	 * endpoint on some newer SoCs (RV1103/RV1106 among them) -- all
	 * subsequent libusb_control_transfer() calls then fail with
	 * LIBUSB_ERROR_IO.
	 */
	if (u->desc.usb_type == RKUSB_MODE_LOADER) {
		fprintf(stderr,
		        "Device is in Loader mode (known PID 0x%04X).\n",
		        u->desc.pid);
		/*
		 * The Rockusb protocol requires only libusb_open → claim_interface
		 * before the first CBW — libusb_clear_halt must not be called here.
		 *
		 * Proactive CLEAR_FEATURE(ENDPOINT_HALT) on both bulk pipes
		 * can make usbplug/Rockusb firmware NAK the next bulk OUT
		 * (first CBW → LIBUSB_ERROR_IO on RV1106 Loader among others).
		 * If a prior session left a real STALL, rkusb / cbw_exec still
		 * clear_halt the stalled pipe when a transfer returns PIPE/IO.
		 */
		return 0;
	}
	if (u->desc.usb_type == RKUSB_MODE_MASKROM) {
		fprintf(stderr,
		        "Device is in MaskROM mode (known PID 0x%04X).\n",
		        u->desc.pid);
		goto handshake;
	}

	/*
	 * Unknown PID: fall back to a runtime probe.  TestUnitReady
	 * against a MaskROM will fail, but for *unknown* PIDs there is no
	 * better option - we still need to know which mode we're in.
	 */
	{
		int loader = rkusb_probe_loader(u);
		if (loader == 1) {
			fprintf(stderr,
			        "Already in Loader mode "
			        "(VID=0x%04X PID=0x%04X, probed).\n",
			        u->desc.vid, u->desc.pid);
			return 0;
		}
	}

	fprintf(stderr,
	        "Device appears to be in MaskROM mode "
	        "(VID=0x%04X PID=0x%04X, probed).\n",
	        u->desc.vid, u->desc.pid);

handshake:
	if (!boot_for_handshake) {
		fprintf(stderr,
		        "No loader supplied - supply UL <loader> or UF <rkfw> "
		        "so the device can enter Loader mode.\n");
		return -EINVAL;
	}

	int rc = maskrom_handshake(u, boot_for_handshake);
	if (rc < 0)
		return rc;

	fprintf(stderr, "Waiting for Loader to come up...\n");
	return wait_for_loader_mode(u);
}

/* ------------------------------------------------------------------ */
/* UL - Upgrade Loader                                                 */
/* ------------------------------------------------------------------ */
static int load_boot_from_path(struct rkboot *out, struct rkfw *fw_out,
                               const char *path)
{
	/*
	 * Accept either a bare RKBOOT (BOOT/LDR) or an RKFW container
	 * that embeds one.  Peek at the first four bytes to decide.
	 */
	FILE *fp = fopen(path, "rb");
	if (!fp) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return -errno;
	}
	char magic[4] = {0};
	size_t m = fread(magic, 1, 4, fp);
	(void)m;
	fclose(fp);

	if (memcmp(magic, "RKFW", 4) == 0) {
		int rc = rkfw_open(fw_out, path);
		if (rc < 0)
			return rc;
		rc = rkfw_load_boot(fw_out);
		if (rc < 0) {
			rkfw_close(fw_out);
			return rc;
		}
		*out = fw_out->boot;
		/* zero the copy in fw_out so close() doesn't double-free */
		memset(&fw_out->boot, 0, sizeof(fw_out->boot));
		rkfw_close(fw_out);
		return 0;
	}
	return rkboot_load_file(out, path);
}

static int cmd_upgrade_loader(int argc, char **argv)
{
	if (argc < 1) {
		fprintf(stderr, "Parameter of [UL] command is invalid.\n");
		return 1;
	}

	struct rkfw   fw   = {0};
	struct rkboot boot = {0};
	int rc = load_boot_from_path(&boot, &fw, argv[0]);
	if (rc < 0) {
		fprintf(stderr, "Can't parse loader from %s (%d)\n",
		        argv[0], rc);
		return 1;
	}
	rkboot_print(&boot);

	struct rkusb u;
	rc = open_and_ensure_loader(&u, &boot);
	if (rc < 0) {
		rkboot_dispose(&boot);
		return 1;
	}

	/*
	 * We're now in Loader mode.  Re-flash the permanent bootloader
	 * slot via the WRITE_LOADER opcode when there are loader
	 * entries; that's what UL is really for once the MaskROM has
	 * already accepted the DDR + first-stage handoff.
	 */
	for (size_t i = 0; i < boot.nloader; ++i) {
		uint8_t *payload = NULL;
		size_t   plen    = 0;
		rc = rkboot_copy_entry(&boot, &boot.loader[i],
		                       &payload, &plen);
		if (rc < 0) {
			fprintf(stderr, "loader[%zu] extract: %d\n", i, rc);
			break;
		}
		rc = rkusb_write_loader_cmd(&u, payload, (uint32_t)plen);
		free(payload);
		if (rc < 0) {
			fprintf(stderr, "loader[%zu] write: %d\n", i, rc);
			break;
		}
		progress("Write loader", i + 1, boot.nloader);
	}
	if (rc == 0) {
		rkusb_reset_device(&u, 0);
		printf("Loader upgrade done.\n");
	}

	rkusb_close(&u);
	rkboot_dispose(&boot);
	return rc ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* DI / UF helpers                                                    */
/* ------------------------------------------------------------------ */
struct dl_ctx {
	struct rkusb *u;
	uint32_t      base_lba;  /* start LBA of the partition on flash */
	const char   *label;
	uint64_t      written;
	uint64_t      total;
};

/* Write `data` (n sectors) at ctx->base_lba + offset_lba, chunking. */
static int dl_write_block(struct dl_ctx *ctx, uint32_t offset_lba,
                          const uint8_t *data, uint32_t sectors)
{
	uint32_t sent = 0;
	while (sent < sectors) {
		uint32_t step = sectors - sent;
		if (step > RKUSB_MAX_LBA_SECTORS)
			step = RKUSB_MAX_LBA_SECTORS;
		int rc = rkusb_write_lba(ctx->u,
		                         ctx->base_lba + offset_lba + sent,
		                         (uint16_t)step,
		                         data + (size_t)sent * RKUSB_SECTOR_BYTES);
		if (rc < 0)
			return rc;
		sent += step;
		ctx->written += (uint64_t)step * RKUSB_SECTOR_BYTES;
		progress(ctx->label, ctx->written, ctx->total);
	}
	return 0;
}

static int dl_sparse_cb(void *user, uint32_t lba, const uint8_t *data,
                        uint32_t sectors, uint32_t fill_word)
{
	struct dl_ctx *ctx = user;
	if (data) {
		return dl_write_block(ctx, lba, data, sectors);
	}
	/* FILL chunk: expand the 32-bit pattern into a sector buffer. */
	uint8_t sec[RKUSB_SECTOR_BYTES];
	for (size_t i = 0; i < sizeof(sec); i += 4)
		memcpy(sec + i, &fill_word, 4);
	while (sectors) {
		uint32_t step = sectors > RKUSB_MAX_LBA_SECTORS
		                ? RKUSB_MAX_LBA_SECTORS : sectors;
		/* Repeat `sec` into a larger buffer for efficiency. */
		static uint8_t scratch[RKUSB_MAX_LBA_SECTORS *
		                       RKUSB_SECTOR_BYTES];
		for (uint32_t s = 0; s < step; ++s)
			memcpy(scratch + (size_t)s * RKUSB_SECTOR_BYTES,
			       sec, RKUSB_SECTOR_BYTES);
		int rc = dl_write_block(ctx, lba, scratch, step);
		if (rc < 0)
			return rc;
		lba     += step;
		sectors -= step;
	}
	return 0;
}

static int download_partition(struct rkusb *u, uint32_t base_lba,
                              const uint8_t *data, size_t data_len,
                              const char *label)
{
	struct dl_ctx ctx = {
		.u        = u,
		.base_lba = base_lba,
		.label    = label,
		.total    = data_len,
	};

	if (rksparse_is_sparse(data, data_len)) {
		/* Total size is unknown in sparse images without parsing;
		 * use the compressed length as a progress denominator. */
		ctx.total = data_len;
		int rc = rksparse_expand(data, data_len, dl_sparse_cb, &ctx);
		if (rc < 0) {
			fprintf(stderr,
			        "sparse expand failed for %s: %d\n",
			        label, rc);
			return rc;
		}
		progress(label, 1, 1);
		return 0;
	}

	/* Raw image: full sector-aligned pad-and-write. */
	size_t total_sectors =
		(data_len + RKUSB_SECTOR_BYTES - 1) / RKUSB_SECTOR_BYTES;
	uint8_t chunk[RKUSB_MAX_LBA_SECTORS * RKUSB_SECTOR_BYTES];
	size_t off = 0;
	uint32_t lba = 0;
	while (total_sectors) {
		size_t step = total_sectors > RKUSB_MAX_LBA_SECTORS
		              ? RKUSB_MAX_LBA_SECTORS : total_sectors;
		size_t want = step * RKUSB_SECTOR_BYTES;
		memset(chunk, 0, want);
		size_t copy = data_len - off;
		if (copy > want) copy = want;
		memcpy(chunk, data + off, copy);

		int rc = rkusb_write_lba(u, base_lba + lba, (uint16_t)step,
		                         chunk);
		if (rc < 0) {
			fprintf(stderr, "WriteLBA failed: %d\n", rc);
			return rc;
		}
		off += copy;
		lba += (uint32_t)step;
		total_sectors -= step;
		progress(label, off, data_len);
	}
	progress(label, data_len, data_len);
	return 0;
}

/* ------------------------------------------------------------------ */
/* UF - Upgrade Firmware                                              */
/* ------------------------------------------------------------------ */
static int cmd_upgrade_firmware(int argc, char **argv)
{
	if (argc < 1) {
		fprintf(stderr, "Parameter of [UF] command is invalid.\n");
		return 1;
	}
	struct rkfw f;
	int rc = rkfw_open(&f, argv[0]);
	if (rc < 0) {
		fprintf(stderr, "open RKFW %s: %d\n", argv[0], rc);
		return 1;
	}
	rkfw_print(&f);

	rc = rkfw_load_boot(&f);
	if (rc < 0) {
		fprintf(stderr, "load boot: %d\n", rc);
		rkfw_close(&f);
		return 1;
	}
	rkboot_print(&f.boot);

	rc = rkfw_load_image(&f);
	if (rc < 0) {
		fprintf(stderr, "load image: %d\n", rc);
		rkfw_close(&f);
		return 1;
	}
	rkaf_print(&f.image);

	struct rkusb u;
	rc = open_and_ensure_loader(&u, &f.boot);
	if (rc < 0)
		goto done;

	/*
	 * GetFlashInfo (0x1a) – first BOT command, mandatory before any writes.
	 *
	 * GetFlashInfo (0x1a) must be the first BOT command — the Rockusb
	 * firmware expects it before any TestUnitReady (TUR).  The BOT handler
	 * is specifically primed to
	 * handle GetFlashInfo first; sending a no-data-phase TUR before the
	 * BOT handler is fully ready triggers a device-side error recovery
	 * that disconnects the device ~50 ms later (LIBUSB_ERROR_NO_DEVICE).
	 *
	 * Retry for up to 5 s (100 × 50 ms) in case usbplug needs a moment
	 * to finish its internal flash controller initialisation.
	 */
	fprintf(stderr, "Get FlashInfo Start\n");
	{
		uint8_t fi[11] = {0};
		bool fi_ok = false;
		for (int i = 0; i < 100; ++i) {
			rc = rkusb_read_flash_info(&u, fi);
			if (rc == 0) {
				fi_ok = true;
				break;
			}
			fprintf(stderr, "  GFI[%d]: %s (%d)\n",
			        i, libusb_error_name(rc), rc);
			if (rc == LIBUSB_ERROR_NO_DEVICE)
				break;
			usleep(50 * 1000);
		}
		if (!fi_ok) {
			fprintf(stderr,
			        "Get FlashInfo failed: %s (%d)\n",
			        libusb_error_name(rc), rc);
			goto out;
		}
		fprintf(stderr,
		        "Get FlashInfo Success"
		        " — mfr=0x%02x type=0x%02x"
		        " blk=%u pg/blk=%u\n",
		        fi[0], fi[1],
		        (unsigned)fi[3] | ((unsigned)fi[4] << 8),
		        (unsigned)fi[5]);
	}

	/* Walk every partition and write it. */
	for (uint32_t i = 0;
	     i < f.image.hdr->num_parts && i < 16;
	     ++i) {
		const struct rkaf_part *p = &f.image.hdr->parts[i];
		if (p->image_size == 0)
			continue;

		/*
		 * Partitions with nand_addr == 0xffffffff are meta-entries
		 * (package-file, bootloader) rather than LBA-flashable.
		 * Both are skipped here: package-file is informational, and
		 * bootloader is redundant because "idblock" (nand_addr 0x200)
		 * already covers the same data via WriteLBA.
		 */
		if (p->nand_addr == 0xffffffffu) {
			fprintf(stderr,
			        "Skipping meta partition %-12.32s (%u B)\n",
			        p->name, p->image_size);
			continue;
		}

		uint8_t *buf = NULL;
		size_t   len = 0;
		rc = rkaf_copy_part(&f.image, p, &buf, &len);
		if (rc < 0) {
			fprintf(stderr, "copy %s: %d\n", p->name, rc);
			goto out;
		}
		uint32_t base = p->nand_addr;
		if (strncmp(p->name, "parameter", sizeof(p->name)) == 0)
			base = 0;
		fprintf(stderr, "\nPartition %-12.32s @ LBA 0x%08x (%zu B)\n",
		        p->name, base, len);
		rc = download_partition(&u, base, buf, len, p->name);
		free(buf);
		if (rc < 0)
			goto out;
	}

	/*
	 * The "bootloader" meta-partition (nand_addr = 0xffffffff) holds the
	 * raw RKBOOT file.  The Rockchip flash tool writes it via a private
	 * PrepareIDB/DownloadIDB path that constructs IDB sectors and writes
	 * them using WRITE_SECTOR (opcode 0x05) at specific flash locations.
	 *
	 * We do not need to replicate that: the "idblock" partition
	 * (flash@LBA = 0x200) already written above contains exactly the same
	 * binary data (FlashHead + FlashData + FlashBoot totalling 188416 B)
	 * at the precise sector offset that the RV1106 BootROM scans.
	 * Writing idblock via WriteLBA is sufficient for the device to boot.
	 *
	 * The WRITE_LOADER command (opcode 0x57) is NOT what the original
	 * tool uses here; sending it to usbplug causes a 20-second timeout.
	 */

	rkusb_reset_device(&u, RKRST_NORMAL);
	printf("Firmware upgrade complete.\n");

out:
	rkusb_close(&u);
done:
	rkfw_close(&f);
	return rc ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* DI - Download Image                                                */
/* ------------------------------------------------------------------ */
static int cmd_download_image(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "Parameter of [DI] command is invalid.\n");
		return 1;
	}
	const char *part_name = argv[0];
	const char *img_path  = argv[1];
	const char *param_path = argc >= 3 ? argv[2] : "parameter.txt";

	struct rk_parameter prm = {0};
	int rc = rk_parameter_load_file(&prm, param_path);
	if (rc < 0) {
		fprintf(stderr, "parameter.txt (%s): %d\n", param_path, rc);
		return 1;
	}

	uint32_t base_lba = 0;
	bool     is_param = false;
	if (strcasecmp(part_name, "parameter") == 0) {
		is_param = true;
	} else if (strcasecmp(part_name, "gpt") == 0) {
		/* "DI gpt <path>" streams the GPT into LBA 0 */
		base_lba = 0;
	} else {
		const struct rk_partition *pp = NULL;
		for (size_t i = 0; i < prm.num_parts; ++i) {
			if (strcasecmp(prm.parts[i].name, part_name) == 0) {
				pp = &prm.parts[i];
				break;
			}
		}
		if (!pp) {
			fprintf(stderr,
			        "Partition '%s' not in parameter.txt.\n",
			        part_name);
			return 1;
		}
		base_lba = (uint32_t)pp->lba_start;
	}

	/* Load the image. */
	FILE *fp = fopen(img_path, "rb");
	if (!fp) {
		fprintf(stderr, "open %s: %s\n", img_path, strerror(errno));
		return 1;
	}
	fseeko(fp, 0, SEEK_END);
	off_t size = ftello(fp);
	fseeko(fp, 0, SEEK_SET);
	uint8_t *buf = malloc((size_t)size);
	if (!buf || fread(buf, 1, (size_t)size, fp) != (size_t)size) {
		fprintf(stderr, "read %s failed\n", img_path);
		free(buf);
		fclose(fp);
		return 1;
	}
	fclose(fp);

	struct rkusb u;
	if (open_and_ensure_loader(&u, NULL) < 0) {
		free(buf);
		return 1;
	}

	if (is_param) {
		/*
		 * parameter.txt is uploaded by writing a small header
		 * ("PARM" + length) followed by the raw file, padded
		 * to 2 KiB and laid down at the first four copies
		 * starting at LBA 0, 0x400, 0x800, 0xc00.  This mirrors
		 * the original tool's behaviour.
		 */
		static const uint32_t copy_at[] = {0x0000, 0x0400,
		                                   0x0800, 0x0c00};
		uint8_t block[2048];
		memset(block, 0, sizeof(block));
		memcpy(block, "PARM", 4);
		uint32_t len = (uint32_t)size;
		memcpy(block + 4, &len, 4);
		size_t copy = size;
		if (copy > sizeof(block) - 12)
			copy = sizeof(block) - 12;
		memcpy(block + 8, buf, copy);
		for (size_t i = 0; i < sizeof(copy_at) / sizeof(*copy_at); ++i) {
			rc = rkusb_write_lba(&u, copy_at[i], 4, block);
			if (rc < 0)
				break;
		}
	} else {
		rc = download_partition(&u, base_lba, buf, (size_t)size,
		                        part_name);
	}

	rkusb_close(&u);
	free(buf);
	return rc ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* GPT - build GPT from parameter.txt                                 */
/* ------------------------------------------------------------------ */
static int cmd_build_gpt(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "Parameter of [GPT] command is invalid.\n");
		return 1;
	}

	struct rk_parameter prm = {0};
	int rc = rk_parameter_load_file(&prm, argv[0]);
	if (rc < 0) {
		fprintf(stderr, "parameter.txt: %d\n", rc);
		return 1;
	}

	/* Probe flash size for the device to size the GPT trailer. */
	uint64_t dev_lbas = 0;
	struct rkusb u;
	if (open_selected(&u) == 0) {
		uint8_t info[11];
		if (rkusb_read_flash_info(&u, info) == 0) {
			uint32_t flash_size = (uint32_t)info[0]
			                    | ((uint32_t)info[1] << 8)
			                    | ((uint32_t)info[2] << 16)
			                    | ((uint32_t)info[3] << 24);
			dev_lbas = flash_size;
		}
		rkusb_close(&u);
	}
	if (dev_lbas == 0) {
		dev_lbas = 0x800000; /* default: 4 GiB */
		fprintf(stderr,
		        "warning: using fallback device size %" PRIu64
		        " sectors\n", dev_lbas);
	}

	uint8_t *blob = NULL;
	size_t   blob_len = 0;
	rc = rk_parameter_build_gpt(&prm, dev_lbas, &blob, &blob_len);
	if (rc < 0) {
		fprintf(stderr, "GPT build failed: %d\n", rc);
		return 1;
	}

	FILE *fp = fopen(argv[1], "wb");
	if (!fp) {
		fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
		free(blob);
		return 1;
	}
	fwrite(blob, 1, blob_len, fp);
	fclose(fp);
	free(blob);
	printf("GPT (%zu bytes, %" PRIu64 " LBA device) written to %s\n",
	       blob_len, dev_lbas, argv[1]);
	return 0;
}

/* ------------------------------------------------------------------ */
/* EF - Erase Flash per parameter.txt                                  */
/* ------------------------------------------------------------------ */
static int cmd_erase_flash(int argc, char **argv)
{
	if (argc < 1) {
		fprintf(stderr, "Parameter of [EF] command is invalid.\n");
		return 1;
	}
	struct rk_parameter prm = {0};
	int rc = rk_parameter_load_file(&prm, argv[0]);
	if (rc < 0) {
		fprintf(stderr, "parameter.txt: %d\n", rc);
		return 1;
	}

	struct rkusb u;
	if (open_and_ensure_loader(&u, NULL) < 0)
		return 1;

	for (size_t i = 0; i < prm.num_parts; ++i) {
		const struct rk_partition *pp = &prm.parts[i];
		uint32_t count = (uint32_t)pp->lba_count;
		if (count == 0)
			continue;
		printf("Erase %-20.36s @ LBA 0x%08lx count 0x%08x\n",
		       pp->name, (unsigned long)pp->lba_start, count);
		int rrc = rkusb_erase_lba(&u, (uint32_t)pp->lba_start, count);
		if (rrc < 0)
			fprintf(stderr, "  failed: %d\n", rrc);
	}
	rkusb_close(&u);
	return 0;
}

/* ------------------------------------------------------------------ */
/* PRINT - introspect an RKFW/RKBOOT/RKAF                             */
/* ------------------------------------------------------------------ */
static int cmd_print(int argc, char **argv)
{
	if (argc < 1) {
		fprintf(stderr, "Parameter of [PRINT] command is invalid.\n");
		return 1;
	}
	const char *path = argv[0];
	FILE *fp = fopen(path, "rb");
	if (!fp) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return 1;
	}
	char magic[4] = {0};
	size_t m = fread(magic, 1, 4, fp);
	(void)m;
	fclose(fp);

	if (memcmp(magic, "RKFW", 4) == 0) {
		struct rkfw f;
		if (rkfw_open(&f, path) < 0) {
			fprintf(stderr, "Invalid RKFW\n");
			return 1;
		}
		rkfw_print(&f);
		if (rkfw_load_boot(&f) == 0)
			rkboot_print(&f.boot);
		if (rkfw_load_image(&f) == 0)
			rkaf_print(&f.image);
		rkfw_close(&f);
		return 0;
	}
	if (memcmp(magic, "BOOT", 4) == 0 || memcmp(magic, "LDR ", 4) == 0) {
		struct rkboot b;
		if (rkboot_load_file(&b, path) < 0) {
			fprintf(stderr, "Invalid RKBOOT\n");
			return 1;
		}
		rkboot_print(&b);
		rkboot_dispose(&b);
		return 0;
	}
	fprintf(stderr, "Unknown container (magic %02x%02x%02x%02x)\n",
	        (uint8_t)magic[0], (uint8_t)magic[1],
	        (uint8_t)magic[2], (uint8_t)magic[3]);
	return 1;
}

/* ------------------------------------------------------------------ */
/* SS - switch storage                                                 */
/* ------------------------------------------------------------------ */
static int cmd_switch_storage(int argc, char **argv)
{
	if (argc < 1) {
		fprintf(stderr, "Parameter of [SS] command is invalid.\n");
		return 1;
	}
	uint8_t storage = 0;
	if (!strcasecmp(argv[0], "flash") || !strcasecmp(argv[0], "nand"))
		storage = RK_STORAGE_FLASH;
	else if (!strcasecmp(argv[0], "emmc"))
		storage = RK_STORAGE_EMMC;
	else if (!strcasecmp(argv[0], "sd"))
		storage = RK_STORAGE_SD;
	else if (!strcasecmp(argv[0], "spinor"))
		storage = RK_STORAGE_SPINOR;
	else if (!strcasecmp(argv[0], "spinand"))
		storage = RK_STORAGE_SPINAND;
	else {
		fprintf(stderr, "Unknown storage '%s'\n", argv[0]);
		return 1;
	}

	struct rkusb u;
	if (open_selected(&u) < 0)
		return 1;
	int rc = rkusb_switch_storage(&u, storage);
	rkusb_close(&u);
	if (rc == 0) {
		printf("Switch storage OK.\n");
		return 0;
	}
	fprintf(stderr, "Switch storage failed: %d\n", rc);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Command dispatch                                                    */
/* ------------------------------------------------------------------ */
struct command {
	const char *name;
	int (*run)(int, char **);
	int min_args;
};

static int cmd_help(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	print_usage();
	return 0;
}

static int cmd_version(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf("%s %s\n", TOOL_NAME, TOOL_VERSION);
	printf("libusb:    %s\n", libusb_get_version()->describe);
	return 0;
}

static int run_td(int a, char **v) { (void)a; (void)v; return cmd_test(); }
static int run_rid(int a, char **v) { (void)a; (void)v; return cmd_read_flash_id(); }
static int run_rfi(int a, char **v) { (void)a; (void)v; return cmd_read_flash_info(); }
static int run_rci(int a, char **v) { (void)a; (void)v; return cmd_read_chip_info(); }
static int run_rcb(int a, char **v) { (void)a; (void)v; return cmd_read_capability(); }

static const struct command COMMANDS[] = {
	{"H",           cmd_help,           0},
	{"-h",          cmd_help,           0},
	{"--help",      cmd_help,           0},
	{"V",           cmd_version,        0},
	{"-v",          cmd_version,        0},
	{"--version",   cmd_version,        0},
	{"LD",          cmd_list,           0},
	{"TD",          run_td,             0},
	{"RD",          cmd_reset,          0},
	{"RP",          cmd_reset_pipe,     0},
	{"RID",         run_rid,            0},
	{"RFI",         run_rfi,            0},
	{"RCI",         run_rci,            0},
	{"RCB",         run_rcb,            0},
	{"RL",          cmd_read_lba,       2},
	{"WL",          cmd_write_lba,      2},
	{"EL",          cmd_erase_lba,      2},
	{"DB",          cmd_download_boot,  1},
	{"UL",          cmd_upgrade_loader, 1},
	{"UF",          cmd_upgrade_firmware, 1},
	{"DI",          cmd_download_image, 2},
	{"GPT",         cmd_build_gpt,      2},
	{"EF",          cmd_erase_flash,    1},
	{"PRINT",       cmd_print,          1},
	{"SS",          cmd_switch_storage, 1},
};

int main(int argc, char **argv)
{
	if (argc < 2) {
		print_usage();
		return 0;
	}

	const char *name = argv[1];
	const struct command *cmd = NULL;
	for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); ++i) {
		if (strcasecmp(COMMANDS[i].name, name) == 0) {
			cmd = &COMMANDS[i];
			break;
		}
	}
	if (!cmd) {
		fprintf(stderr,
		        "command is invalid, please press '%s -h' to check usage!\n",
		        TOOL_NAME);
		return 1;
	}

	int sub_argc = argc - 2;
	char **sub_argv = argv + 2;
	if (sub_argc < cmd->min_args) {
		fprintf(stderr, "Parameter of [%s] command is invalid.\n",
		        cmd->name);
		return 1;
	}

	if (rkusb_library_init() < 0)
		return 1;

	int rc = cmd->run(sub_argc, sub_argv);

	rkusb_library_exit();
	return rc;
}
