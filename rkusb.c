/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2024 rockutil contributors
 */
/*
 * rkusb.c - libusb-1.0 backed Rockchip Rockusb communication layer.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "rkusb.h"
#include "rkcrc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Static state                                                       */
/* ------------------------------------------------------------------ */
static libusb_context *g_ctx;
static bool            g_seeded;

/*
 * Table of (vid, pid, device_type, usb_type) tuples covering all known
 * Rockchip SoC USB IDs published in Rockchip open-source SDKs and
 * board support packages.
 */
struct rkdev_entry {
	uint16_t vid;
	uint16_t pid;
	uint16_t device_type;
	uint16_t usb_type;
	const char *nickname;
};

static const struct rkdev_entry KNOWN_DEVICES[] = {
	/* old Fuzhou Rockchip VID 0x071b - MaskROM */
	{FUZHOU_VID, 0x3210, RKDEV_RK27,     RKUSB_MODE_MASKROM, "RK27 maskrom"   },
	{FUZHOU_VID, 0x3228, RKDEV_RK28,     RKUSB_MODE_MASKROM, "RK28 maskrom"   },
	{FUZHOU_VID, 0x3226, RKDEV_RKCAYMAN, RKUSB_MODE_MASKROM, "RKCayman mskrm" },
	/* current Rockchip VID 0x2207 - MaskROM + Loader */
	{ROCKCHIP_VID, 0x261a, RKDEV_SMART,    RKUSB_MODE_MASKROM, "RKSmart mskrm"    },
	{ROCKCHIP_VID, 0x281a, RKDEV_RK281X,   RKUSB_MODE_MASKROM, "RK281X mskrm"     },
	{ROCKCHIP_VID, 0x273a, RKDEV_RK27B,    RKUSB_MODE_MASKROM, "RK27B mskrm"      },
	{ROCKCHIP_VID, 0x290a, RKDEV_RK29,     RKUSB_MODE_MASKROM, "RK29 mskrm"       },
	{ROCKCHIP_VID, 0x282b, RKDEV_RKNAND,   RKUSB_MODE_MASKROM, "RKNAND mskrm"     },
	{ROCKCHIP_VID, 0x262c, RKDEV_RK26,     RKUSB_MODE_MASKROM, "RK26 mskrm"       },
	{ROCKCHIP_VID, 0x292a, RKDEV_RK292X,   RKUSB_MODE_MASKROM, "RK292X mskrm"     },
	{ROCKCHIP_VID, 0x300a, RKDEV_RK30,     RKUSB_MODE_MASKROM, "RK30 mskrm"       },
	{ROCKCHIP_VID, 0x300b, RKDEV_RK30B,    RKUSB_MODE_MASKROM, "RK30B mskrm"      },
	{ROCKCHIP_VID, 0x310b, RKDEV_RK31,     RKUSB_MODE_MASKROM, "RK31 mskrm"       },
	{ROCKCHIP_VID, 0x320a, RKDEV_RK32,     RKUSB_MODE_MASKROM, "RK32 mskrm"       },
	{ROCKCHIP_VID, 0x320b, RKDEV_RK32,     RKUSB_MODE_LOADER,  "RK322X loader"    },
	{ROCKCHIP_VID, 0x330a, RKDEV_RK33,     RKUSB_MODE_MASKROM, "RK33 mskrm"       },
	{ROCKCHIP_VID, 0x330c, RKDEV_RK35,     RKUSB_MODE_MASKROM, "RV1126 mskrm"     },
	/* RK35xx / RK356x / RK3588 family */
	{ROCKCHIP_VID, 0x350a, RKDEV_RK35,     RKUSB_MODE_MASKROM, "RK35xx mskrm"     },
	{ROCKCHIP_VID, 0x350b, RKDEV_RK35,     RKUSB_MODE_LOADER,  "RK35xx loader"    },
	{ROCKCHIP_VID, 0x350c, RKDEV_RK35,     RKUSB_MODE_MASKROM, "RK3588 mskrm"     },
	{ROCKCHIP_VID, 0x350d, RKDEV_RK35,     RKUSB_MODE_LOADER,  "RK3588 loader"    },
	{ROCKCHIP_VID, 0x350e, RKDEV_RK35,     RKUSB_MODE_MASKROM, "RK3576 mskrm"     },
	{ROCKCHIP_VID, 0x3588, RKDEV_RK35,     RKUSB_MODE_MASKROM, "RK3588 mskrm-alt" },
	/* RV1103 / RV1106 (Luckfox Pico family) */
	{ROCKCHIP_VID, 0x110a, RKDEV_RK35,     RKUSB_MODE_MASKROM, "RV1103 mskrm"     },
	{ROCKCHIP_VID, 0x110b, RKDEV_RK35,     RKUSB_MODE_LOADER,  "RV1103 loader"    },
	{ROCKCHIP_VID, 0x110c, RKDEV_RK35,     RKUSB_MODE_MASKROM, "RV1106 mskrm"     },
	{ROCKCHIP_VID, 0x110d, RKDEV_RK35,     RKUSB_MODE_LOADER,  "RV1106 loader"    },
	/*
	 * The real tool also learns additional (VID, PID) pairs by walking
	 * its config.ini at startup.  In addition, runtime mode detection
	 * via rkusb_probe_loader() lets us distinguish MaskROM from Loader
	 * even when the PID is unknown.
	 */
};

/* ------------------------------------------------------------------ */
/* Library lifecycle                                                  */
/* ------------------------------------------------------------------ */
int rkusb_library_init(void)
{
	if (g_ctx)
		return 0;

	int rc = libusb_init(&g_ctx);
	if (rc < 0) {
		fprintf(stderr, "libusb_init failed: %s\n",
		        libusb_error_name(rc));
		g_ctx = NULL;
		return rc;
	}

	if (!g_seeded) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		srand((unsigned)(ts.tv_sec ^ ts.tv_nsec));
		g_seeded = true;
	}
	return 0;
}

void rkusb_library_exit(void)
{
	if (g_ctx) {
		libusb_exit(g_ctx);
		g_ctx = NULL;
	}
}

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */
static const struct rkdev_entry *lookup_device_entry(uint16_t vid,
                                                     uint16_t pid)
{
	for (size_t i = 0; i < sizeof(KNOWN_DEVICES) / sizeof(KNOWN_DEVICES[0]);
	     ++i) {
		if (KNOWN_DEVICES[i].vid == vid && KNOWN_DEVICES[i].pid == pid)
			return &KNOWN_DEVICES[i];
	}
	return NULL;
}

/*
 * Walk the active config descriptor looking for a vendor-specific
 * Rockusb interface (class 0xff, subclass 0x06, protocol 0x05) and
 * return its number plus bulk IN / OUT endpoint addresses.
 */
/*
 * Locate the Rockusb bulk interface on a Rockchip device.
 * Accepts:
 *   1. Vendor class 0xFF / subclass 0x06 / protocol 0x05  (Rockusb)
 *   2. MSC-compatible  0x08 / 0x06 / 0x50                 (some Loaders)
 *
 * The function returns 0 on success, LIBUSB_ERROR_NOT_FOUND otherwise.
 */
static int locate_rockusb_interface(libusb_device *dev, int *iface,
                                    uint8_t *ep_in, uint8_t *ep_out)
{
	struct libusb_config_descriptor *cfg = NULL;
	int rc = libusb_get_active_config_descriptor(dev, &cfg);
	if (rc < 0)
		return rc;

	int found = LIBUSB_ERROR_NOT_FOUND;
	for (uint8_t i = 0; i < cfg->bNumInterfaces; ++i) {
		const struct libusb_interface *intf = &cfg->interface[i];
		for (int a = 0; a < intf->num_altsetting; ++a) {
			const struct libusb_interface_descriptor *d =
				&intf->altsetting[a];
			bool rockusb = (d->bInterfaceClass == 0xff &&
			                d->bInterfaceSubClass == 0x06 &&
			                d->bInterfaceProtocol == 0x05);
			bool msc = (d->bInterfaceClass == 0x08 &&
			            d->bInterfaceSubClass == 0x06 &&
			            d->bInterfaceProtocol == 0x50);
			if (!rockusb && !msc)
				continue;

			uint8_t in = 0, out = 0;
			for (uint8_t e = 0; e < d->bNumEndpoints; ++e) {
				uint8_t addr = d->endpoint[e].bEndpointAddress;
				uint8_t attr = d->endpoint[e].bmAttributes;
				if ((attr & 0x03) != LIBUSB_TRANSFER_TYPE_BULK)
					continue;
				if (addr & 0x80)
					in = addr;
				else
					out = addr;
			}
			if (in && out) {
				*iface = d->bInterfaceNumber;
				*ep_in = in;
				*ep_out = out;
				found = 0;
				goto done;
			}
		}
	}
done:
	libusb_free_config_descriptor(cfg);
	return found;
}

/* ------------------------------------------------------------------ */
/* Enumeration                                                        */
/* ------------------------------------------------------------------ */
int rkusb_enumerate(struct rkdev_list *out)
{
	out->devs = NULL;
	out->count = 0;

	libusb_device **list = NULL;
	ssize_t n = libusb_get_device_list(g_ctx, &list);
	if (n < 0)
		return (int)n;

	size_t cap = 0;
	for (ssize_t i = 0; i < n; ++i) {
		libusb_device *dev = list[i];
		struct libusb_device_descriptor d;
		if (libusb_get_device_descriptor(dev, &d) < 0)
			continue;

		const struct rkdev_entry *entry =
			lookup_device_entry(d.idVendor, d.idProduct);
		int iface;
		uint8_t ep_in, ep_out;
		int has_iface = locate_rockusb_interface(dev, &iface,
		                                         &ep_in, &ep_out);
		/* Only use interface-based detection for known Rockchip VIDs.
		 * This prevents matching non-Rockchip vendor-class devices
		 * (e.g. fingerprint readers, cameras) that happen to expose
		 * a 0xFF class or bulk endpoints. */
		bool is_rk_vid = (d.idVendor == ROCKCHIP_VID ||
		                  d.idVendor == FUZHOU_VID ||
		                  d.idVendor == BBK_VID);
		if (!entry && (!is_rk_vid || has_iface < 0))
			continue;

		if (out->count == cap) {
			cap = cap ? cap * 2 : 4;
			out->devs = realloc(out->devs,
			                    cap * sizeof(*out->devs));
			if (!out->devs) {
				libusb_free_device_list(list, 1);
				return LIBUSB_ERROR_NO_MEM;
			}
		}

		struct rkdev_desc *r = &out->devs[out->count++];
		memset(r, 0, sizeof(*r));
		r->dev           = libusb_ref_device(dev);
		r->vid           = d.idVendor;
		r->pid           = d.idProduct;
		r->bcdUSB        = d.bcdUSB;
		r->iManufacturer = d.iManufacturer;
		r->iProduct      = d.iProduct;
		r->device_type   = entry ? entry->device_type : RKDEV_NONE;

		/*
		 * Mode classification — step 1: PID table / interface detection.
		 *
		 * The KNOWN_DEVICES table is the primary authority for known PIDs.
		 * Interface detection is the fallback for unknown PIDs only.
		 *
		 * Background: newer Rockchip SoCs (RV1103/RV1106, RK3588 family)
		 * expose the full class-0xFF/0x06/0x05 Rockusb bulk interface even
		 * in MaskROM mode, so interface presence cannot distinguish the two
		 * modes — the table must be trusted for known PIDs.
		 */
		if (entry)
			r->usb_type = entry->usb_type;
		else if (has_iface >= 0)
			r->usb_type = RKUSB_MODE_LOADER;
		else
			r->usb_type = RKUSB_MODE_MASKROM;

		/*
		 * Mode classification — step 2: string-descriptor tiebreaker.
		 *
		 * Some SoCs (observed on RV1106) use the SAME PID for both
		 * MaskROM and the usbplug loader:
		 *
		 *   MaskROM  (device 105 in dmesg): PID=0x110C, iMfr=0, iProd=0
		 *   usbplug  (device 106 in dmesg): PID=0x110C, iMfr=1, iProd=2
		 *                                   Product="USB-MSC" Mfr="RockChip"
		 *
		 * The table alone classifies both as MASKROM (0x110C entry).
		 * The USB device descriptor fields iManufacturer and iProduct are
		 * string-descriptor INDICES, not the string content.  Reading them
		 * via libusb_get_device_descriptor() requires NO open() call and
		 * does not disturb the device.
		 *
		 * Rockchip MaskROM firmware never configures string descriptors:
		 * iManufacturer=0, iProduct=0, iSerialNumber=0.
		 * Rockchip usbplug/Loader firmware always configures at least one
		 * non-zero string index.
		 *
		 * Therefore: if the table says MASKROM but the device descriptor
		 * has a non-zero iManufacturer or iProduct, the device is actually
		 * running usbplug and should be classified as LOADER.
		 */
		if (r->usb_type == RKUSB_MODE_MASKROM &&
		    (d.iManufacturer != 0 || d.iProduct != 0))
			r->usb_type = RKUSB_MODE_LOADER;

		r->bus = libusb_get_bus_number(dev);
		int plen = libusb_get_port_numbers(dev, r->port_path,
		                                   sizeof(r->port_path));
		r->port_path_len = plen > 0 ? plen : 0;
		r->location_id = (uint32_t)r->bus << 24;
		for (int p = 0; p < r->port_path_len; ++p)
			r->location_id |= (uint32_t)r->port_path[p] << (p * 4);
		/*
		 * Serial number string intentionally omitted: reading it
		 * requires libusb_open(), and opening the device during
		 * polling (before the real open) disturbs usbplug.
		 */
	}

	libusb_free_device_list(list, 1);
	return 0;
}

void rkusb_free_list(struct rkdev_list *list)
{
	if (!list)
		return;
	for (size_t i = 0; i < list->count; ++i)
		if (list->devs[i].dev)
			libusb_unref_device(list->devs[i].dev);
	free(list->devs);
	list->devs = NULL;
	list->count = 0;
}

const char *rkusb_mode_name(uint16_t mode)
{
	switch (mode) {
	case RKUSB_MODE_MASKROM: return "Maskrom";
	case RKUSB_MODE_LOADER:  return "Loader";
	case RKUSB_MODE_MSC:     return "MSC";
	case RKUSB_MODE_UVC:     return "UVC";
	default:                 return "Unknown";
	}
}

const char *rkusb_device_type_name(uint16_t t)
{
	switch (t) {
	case RKDEV_RK27:     return "RK27";
	case RKDEV_RK27B:    return "RK27B";
	case RKDEV_RK28:     return "RK28";
	case RKDEV_RKCAYMAN: return "RKCayman";
	case RKDEV_RK281X:   return "RK281X";
	case RKDEV_RKNAND:   return "RKNAND";
	case RKDEV_RK26:     return "RK26";
	case RKDEV_SMART:    return "RKSmart";
	case RKDEV_RK29:     return "RK29";
	case RKDEV_RK292X:   return "RK292X";
	case RKDEV_RK30:     return "RK30";
	case RKDEV_RK30B:    return "RK30B";
	case RKDEV_RK31:     return "RK31";
	case RKDEV_RK32:     return "RK32";
	case RKDEV_RK33:     return "RK33";
	case RKDEV_RK35:     return "RK35";
	default:             return "Unknown";
	}
}

/* ------------------------------------------------------------------ */
/* Open / close a device                                              */
/* ------------------------------------------------------------------ */
int rkusb_open(struct rkusb *u, const struct rkdev_desc *desc)
{
	memset(u, 0, sizeof(*u));
	u->desc = *desc;
	if (u->desc.dev)
		u->desc.dev = libusb_ref_device(u->desc.dev);
	u->timeout_ms = RKUSB_DEFAULT_TIMEOUT_MS;
	u->interface = -1;

	int rc = libusb_open(desc->dev, &u->handle);
	if (rc < 0)
		return rc;

	rc = locate_rockusb_interface(desc->dev, &u->interface,
	                              &u->ep_in, &u->ep_out);
	if (rc < 0) {
		libusb_close(u->handle);
		u->handle = NULL;
		return rc;
	}

	/*
	 * Claim the interface directly — NO libusb_kernel_driver_active /
	 * libusb_detach_kernel_driver call first.
	 *
	 * Rockusb devices are accessed via libusb_open →
	 * libusb_get_active_config_descriptor → interface scan →
	 * libusb_free_config_descriptor → libusb_claim_interface.
	 * There is NO kernel-driver detach in this path.
	 *
	 * If we call libusb_kernel_driver_active + libusb_detach_kernel_driver
	 * before claiming, the kernel sends USBDEVFS_DISCONNECT to whatever
	 * driver is attached (e.g. usb-storage on MSC-class Loaders).  Some
	 * driver versions issue a USB bus reset as part of their disconnect
	 * handler.  That reset fires asynchronously — libusb_claim_interface
	 * still returns 0 because the claim completes before the reset, but
	 * the very next bulk transfer returns LIBUSB_ERROR_NO_DEVICE (-4)
	 * because the device has already disconnected.
	 *
	 * If no kernel driver is bound (the normal case for class 0xFF/0x06/
	 * 0x05 Rockusb devices), libusb_claim_interface succeeds immediately.
	 * If a driver IS bound, it returns LIBUSB_ERROR_BUSY (-6) and the
	 * caller can decide whether to detach — but that scenario is handled
	 * by adding a udev rule instead.
	 */
	rc = libusb_claim_interface(u->handle, u->interface);
	if (rc < 0) {
		libusb_close(u->handle);
		u->handle = NULL;
		return rc;
	}
	return 0;
}

/*
 * Build the sysfs device directory path (e.g. "/sys/bus/usb/devices/3-6"
 * or "/sys/bus/usb/devices/3-1.2") into `buf` (size `bufsz`).
 * Returns 0 on success, -1 if port_path_len is 0 (path not known).
 */
static int sysfs_device_path(const struct rkdev_desc *desc,
                              char *buf, size_t bufsz)
{
	if (desc->port_path_len <= 0)
		return -1;

	/* Allow tests to redirect the sysfs tree to a temporary directory. */
	const char *sysfs_root = getenv("ROCKUTIL_SYSFS_ROOT");
	if (!sysfs_root)
		sysfs_root = "";

	int pos = snprintf(buf, bufsz, "%s/sys/bus/usb/devices/%u",
	                   sysfs_root, desc->bus);
	for (int i = 0; i < desc->port_path_len && pos < (int)bufsz - 1; ++i) {
		pos += snprintf(buf + pos, bufsz - pos,
		                "%s%u",
		                i == 0 ? "-" : ".",
		                desc->port_path[i]);
	}
	return 0;
}

/*
 * Parse a raw USB descriptor blob (device descriptor at offset 0,
 * config descriptor(s) immediately after) for a Rockusb bulk interface
 * (class=0xFF, subClass=6, protocol=5) with at least one bulk-IN and
 * one bulk-OUT endpoint.
 *
 * Returns 0 and fills *iface_num / *ep_in / *ep_out on success.
 * Returns -1 if no qualifying interface is found.
 */
static int parse_rockusb_interface(const uint8_t *buf, int len,
                                   int *iface_num,
                                   uint8_t *ep_in, uint8_t *ep_out)
{
	int     cur_iface = -1;
	uint8_t cur_ep_in = 0, cur_ep_out = 0;

	for (int pos = 0; pos + 2 <= len; ) {
		int     dlen  = buf[pos];
		uint8_t dtype = buf[pos + 1];

		if (dlen < 2 || pos + dlen > len)
			break;

		if (dtype == LIBUSB_DT_INTERFACE && dlen >= 9) {
			if (buf[pos + 5] == 0xFF &&
			    buf[pos + 6] == 6 &&
			    buf[pos + 7] == 5) {
				cur_iface  = buf[pos + 2];
				cur_ep_in  = 0;
				cur_ep_out = 0;
			} else {
				cur_iface = -1;
			}
		} else if (dtype == LIBUSB_DT_ENDPOINT && dlen >= 7 &&
		           cur_iface >= 0) {
			uint8_t addr = buf[pos + 2];
			uint8_t attr = buf[pos + 3];
			if ((attr & 0x03) == LIBUSB_TRANSFER_TYPE_BULK) {
				if (addr & LIBUSB_ENDPOINT_IN)
					cur_ep_in  = addr;
				else
					cur_ep_out = addr;
			}
		}
		pos += dlen;
	}

	if (cur_iface < 0 || cur_ep_in == 0 || cur_ep_out == 0)
		return -1;

	*iface_num = cur_iface;
	*ep_in     = cur_ep_in;
	*ep_out    = cur_ep_out;
	return 0;
}

/*
 * Open a device by reading its descriptor from the kernel sysfs
 * "descriptors" file rather than from libusb's stale internal cache or
 * via a USB control transfer.
 *
 * Background:
 *   Some Rockchip SoCs transition from MaskROM to usbplug via an
 *   in-place USB bus reset - same bus/device address.  The kernel
 *   re-reads the device and config descriptors (iManufacturer changes
 *   from 0 to 1, bulk endpoints appear) and updates sysfs in-place.
 *   The udev event type is "change" or "bind", which libusb ignores:
 *   ctx->usb_devs retains the stale MaskROM entry, and all libusb
 *   descriptor APIs return old data.  (The RV1106 physically
 *   disconnects instead, but this function is still useful as a
 *   non-intrusive open path once the device is already enumerated.)
 *
 *   Sending GET_DESCRIPTOR control transfers to detect the transition
 *   (the previous approach) is also unreliable: opening the USB device
 *   fd and sending USB traffic appears to prevent usbplug from
 *   completing its USB stack initialisation.
 *
 *   Reading /sys/bus/usb/devices/BUS-PATH/descriptors is entirely
 *   non-intrusive — it does not open the USB device fd, does not send
 *   any USB traffic, and always returns the kernel's current view of
 *   the descriptor.  It is safe to call every 50 ms while waiting.
 *
 * Algorithm:
 *   1. Build the sysfs path from desc->bus + desc->port_path.
 *   2. Read the raw descriptor blob; byte 14 is iManufacturer.
 *      If iManufacturer==0 && iProduct==0 → still MaskROM → -ENODEV.
 *   3. Skip past the 18-byte device descriptor and parse the config
 *      descriptor to locate the 0xFF/6/5 bulk interface.
 *   4. Open the USB device fd with libusb_open (now safe — the device
 *      is confirmed to be usbplug and its USB stack is ready) and
 *      claim the interface.
 *
 * Returns 0 on success, -ENODEV if the sysfs read is unavailable or
 * shows MaskROM, or another negative libusb error on open/claim failure.
 */
int rkusb_open_live(struct rkusb *u, const struct rkdev_desc *desc)
{
	/* Step 1: build sysfs path. */
	char sysfs_dir[256];
	if (sysfs_device_path(desc, sysfs_dir, sizeof(sysfs_dir)) < 0)
		return -ENODEV;

	char descs_path[280];
	snprintf(descs_path, sizeof(descs_path), "%s/descriptors", sysfs_dir);

	/* Step 2: read the raw descriptor blob (no USB fd open, no traffic). */
	FILE *f = fopen(descs_path, "rb");
	if (!f)
		return -ENODEV;

	uint8_t raw[640]; /* device desc (18) + typical config desc (<600) */
	int raw_len = (int)fread(raw, 1, sizeof(raw), f);
	fclose(f);

	if (raw_len < 18)
		return -ENODEV;

	uint8_t imfr  = raw[14]; /* iManufacturer index */
	uint8_t iprod = raw[15]; /* iProduct index      */
	if (imfr == 0 && iprod == 0)
		return -ENODEV; /* still MaskROM */

	/* Step 3: parse config descriptor (starts after the 18-byte device desc). */
	int     iface_num = -1;
	uint8_t ep_in = 0, ep_out = 0;
	if (parse_rockusb_interface(raw + 18, raw_len - 18,
	                             &iface_num, &ep_in, &ep_out) < 0)
		return -ENODEV;

	/* Step 4: now safe to open the USB device — usbplug is confirmed ready. */
	memset(u, 0, sizeof(*u));
	u->interface = -1;

	int rc = libusb_open(desc->dev, &u->handle);
	if (rc < 0)
		return rc;

	rc = libusb_claim_interface(u->handle, iface_num);
	if (rc < 0) {
		libusb_close(u->handle);
		u->handle = NULL;
		return rc;
	}

	u->interface  = iface_num;
	u->ep_in      = ep_in;
	u->ep_out     = ep_out;
	u->timeout_ms = RKUSB_DEFAULT_TIMEOUT_MS;
	u->desc       = *desc;
	if (u->desc.dev)
		u->desc.dev = libusb_ref_device(u->desc.dev);
	u->desc.usb_type      = RKUSB_MODE_LOADER;
	u->desc.iManufacturer = imfr;
	u->desc.iProduct      = iprod;
	return 0;
}

void rkusb_close(struct rkusb *u)
{
	if (!u)
		return;
	if (u->handle) {
		if (u->interface >= 0)
			libusb_release_interface(u->handle, u->interface);
		libusb_close(u->handle);
	}
	if (u->desc.dev)
		libusb_unref_device(u->desc.dev);
	memset(u, 0, sizeof(*u));
	u->interface = -1;
}

int rkusb_reset_pipe(struct rkusb *u, uint8_t pipe)
{
	uint8_t ep = (pipe == 0) ? u->ep_out : u->ep_in;
	return libusb_clear_halt(u->handle, ep);
}

/* ------------------------------------------------------------------ */
/* Bulk-only-transport helpers                                         */
/* ------------------------------------------------------------------ */
static uint32_t make_tag(void)
{
	/*
	 * Mirrors CRKUsbComm::MakeCBWTag(): four 8-bit iterations that
	 * scale rand() by RAND_MAX and shift into the accumulator.  The
	 * net effect is a 32-bit value that isn't cryptographically
	 * meaningful but is plenty unique for CBW/CSW matching.
	 */
	uint32_t v = 0;
	for (int i = 0; i < 4; ++i)
		v = (v << 8) | (uint32_t)(rand() & 0xff);
	return v;
}

/*
 * Build a CBW from a logical operation.  direction is 0=OUT, 1=IN.
 * The Rockchip opcode table (derived from InitializeCBW in the PE)
 * pairs every opcode with a canonical CDB length; rely on it so we
 * match the firmware's expectations byte-for-byte.
 */
struct rkop_info {
	uint8_t dir;     /* 1 = IN (device->host)     */
	uint8_t cdb_len; /* 6 or 10                   */
};

static const struct rkop_info OP_TABLE[] = {
	[RKOP_TEST_UNIT_READY]  = {0, 6},
	[RKOP_READ_FLASH_ID]    = {1, 6},
	[RKOP_SET_DEVICE_ID]    = {0, 6},
	[RKOP_TEST_BAD_BLOCK]   = {1, 10},
	[RKOP_READ_SECTOR]      = {1, 10},
	[RKOP_WRITE_SECTOR]     = {0, 10},
	[RKOP_ERASE_NORMAL]     = {0, 10},
	[RKOP_ERASE_FORCE]      = {0, 10},
	[RKOP_READ_LBA]         = {1, 10},
	[RKOP_WRITE_LBA]        = {0, 10},
	[RKOP_ERASE_SYSTEMDISK] = {0,  6},
	[RKOP_READ_SDRAM]       = {1, 10},
	[RKOP_WRITE_SDRAM]      = {0, 10},
	[RKOP_EXECUTE_SDRAM]    = {0, 10},
	[RKOP_READ_FLASH_INFO]  = {1,  6},
	[RKOP_READ_CHIP_INFO]   = {1,  6},
	[RKOP_SET_RESET_FLAG]   = {0,  6},
	[RKOP_WRITE_EFUSE]      = {0,  6},
	[RKOP_READ_EFUSE]       = {1,  6},
	[RKOP_READ_SPARE]       = {1, 10},
	[RKOP_WRITE_SPARE]      = {0, 10},
	[RKOP_ERASE_LBA]        = {0, 10},
	/*
	 * These opcodes are above RKOP_ERASE_LBA (0x25) and therefore
	 * outside the dense part of the table.  They are listed here so
	 * that init_cbw's fallback "data_len ? IN : OUT" heuristic is
	 * never reached for them.  (The designated-initialiser syntax
	 * zero-fills the gaps between 0x25 and these entries; those gaps
	 * are never consulted because the fallback branch checks
	 * OP_TABLE[opcode].cdb_len != 0 first.)
	 */
	[RKOP_VS_READ]          = {1,  6},   /* 0x26 vendor storage read  */
	[RKOP_VS_WRITE]         = {0,  6},   /* 0x27 vendor storage write */
	[RKOP_ERASE_SECTORS]    = {0,  6},   /* 0x29 */
	[RKOP_CHANGE_STORAGE]   = {0,  6},   /* 0x2b */
	[RKOP_OTP_READ]         = {1,  6},   /* 0x2c OTP/eFuse read       */
	[RKOP_READ_VENDOR]      = {1,  6},   /* 0x56 */
	[RKOP_WRITE_LOADER]     = {0,  6},   /* 0x57 OUT: host writes to device */
};

static void init_cbw(struct cbw *cbw, uint8_t opcode, uint32_t data_len)
{
	memset(cbw, 0, sizeof(*cbw));
	cbw->signature   = CBW_SIGNATURE;
	cbw->tag         = make_tag();
	cbw->data_length = data_len;
	cbw->lun         = 0;

	if (opcode == RKOP_READ_CAPABILITY) {
		cbw->flags      = 0x80;   /* IN: device sends 8 bytes */
		cbw->cdb_length = 6;
	} else if (opcode == RKOP_DEVICE_RESET) {
		cbw->flags      = 0x00;   /* no data transfer */
		cbw->cdb_length = 6;
	} else if (opcode < sizeof(OP_TABLE) / sizeof(OP_TABLE[0]) &&
	           OP_TABLE[opcode].cdb_len) {
		cbw->flags      = OP_TABLE[opcode].dir ? 0x80 : 0x00;
		cbw->cdb_length = OP_TABLE[opcode].cdb_len;
	} else {
		/* conservative default */
		cbw->flags      = data_len ? 0x80 : 0x00;
		cbw->cdb_length = 6;
	}
	cbw->cdb[0] = opcode;
}

static int bulk(struct rkusb *u, uint8_t ep, uint8_t *buf, int len)
{
	int transferred = 0;
	int rc = libusb_bulk_transfer(u->handle, ep, buf, len,
	                              &transferred, u->timeout_ms);
	if (rc < 0)
		return rc;
	if (transferred != len)
		return LIBUSB_ERROR_IO;
	return 0;
}

static void clear_halt_both(struct rkusb *u)
{
	libusb_clear_halt(u->handle, u->ep_in);
	libusb_clear_halt(u->handle, u->ep_out);
}

/*
 * Send one command: optional data stage, then read back the CSW and
 * validate that the tag + residue match.
 *
 * On pipe stalls the USB MSC BoT spec requires Clear Feature HALT on
 * the offending endpoint before we try to read the CSW.  We do that
 * here so a single failed opcode doesn't wedge subsequent ones.
 */
static int cbw_exec(struct rkusb *u, struct cbw *cbw,
                    uint8_t *data, uint32_t data_len)
{
	int data_rc = 0;

	/*
	 * Retry CBW bulk-out on LIBUSB_ERROR_IO.
	 *
	 * The Loader firmware may not be ready to accept bulk OUT
	 * transfers immediately after USB enumeration — it can take up to
	 * ~200 ms for the USB stack to initialise its endpoints.  A plain
	 * sleep between attempts is the correct remedy.
	 *
	 * Do NOT call libusb_clear_halt between retries.  The Rockchip
	 * Loader/usbplug firmware treats an unexpected CLEAR_FEATURE
	 * (ENDPOINT_HALT) control request as a protocol violation and
	 * begins NAKing all subsequent bulk OUT packets — turning a
	 * transient startup error into a permanent one.
	 *
	 * Since no CBW has been accepted yet the device's BOT state
	 * machine has not advanced, so re-sending the same CBW is safe.
	 *
	 * 10 attempts × 50 ms = up to 500 ms of patience, which covers
	 * every Rockchip Loader variant seen in the field.
	 */
	int rc = LIBUSB_ERROR_IO;
	for (int attempt = 0; attempt < 10; ++attempt) {
		rc = bulk(u, u->ep_out, (uint8_t *)cbw, sizeof(*cbw));
		if (rc == 0)
			break;
		if (rc != LIBUSB_ERROR_IO)
			break;
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000L };
		nanosleep(&ts, NULL);
	}
	if (rc < 0) {
		fprintf(stderr, "  cbw_exec: CBW write failed op=0x%02x: %s (%d)\n",
		        cbw->cdb[0], libusb_error_name(rc), rc);
		return rc;
	}

	if (data_len && data) {
		uint8_t ep = (cbw->flags & 0x80) ? u->ep_in : u->ep_out;
		data_rc = bulk(u, ep, data, (int)data_len);
		if (data_rc == LIBUSB_ERROR_PIPE) {
			libusb_clear_halt(u->handle, ep);
		} else if (data_rc < 0 && data_rc != LIBUSB_ERROR_IO) {
			/*
			 * Hard error (e.g. LIBUSB_ERROR_NO_DEVICE): the
			 * device is gone and we cannot recover the CSW.
			 * Return immediately.
			 */
			return data_rc;
		}
		/*
		 * LIBUSB_ERROR_IO means a short packet (transferred < len).
		 * Per USB MSC BOT spec the host MUST still read the CSW
		 * after a short data-in transfer.  Fall through so we always
		 * consume the CSW — otherwise the device's BOT state machine
		 * goes out of phase and the next CBW looks corrupt, causing
		 * a USB reset.
		 */
	}

	struct csw csw;
	int csw_rc = bulk(u, u->ep_in, (uint8_t *)&csw, sizeof(csw));
	if (csw_rc == LIBUSB_ERROR_PIPE) {
		libusb_clear_halt(u->handle, u->ep_in);
		csw_rc = bulk(u, u->ep_in, (uint8_t *)&csw, sizeof(csw));
	}
	if (csw_rc < 0) {
		fprintf(stderr, "  cbw_exec: CSW read failed op=0x%02x: %s (%d)\n",
		        cbw->cdb[0], libusb_error_name(csw_rc), csw_rc);
		/*
		 * Do NOT call clear_halt_both() here.
		 *
		 * If the CSW read failed because the device is not yet ready
		 * (e.g. usbplug still initialising: short/empty packet →
		 * LIBUSB_ERROR_IO, or device disconnected → LIBUSB_ERROR_NO_DEVICE),
		 * sending CLEAR_FEATURE(ENDPOINT_HALT) to both endpoints is
		 * FATAL: the usbplug firmware interprets the unexpected control
		 * requests as a protocol violation and immediately resets its
		 * USB connection.  The caller then sees LIBUSB_ERROR_NO_DEVICE
		 * on the very next transfer.
		 *
		 * clear_halt_both() should only be called to recover from a
		 * genuine endpoint STALL (LIBUSB_ERROR_PIPE), which is already
		 * handled by the PIPE check above this block.  For all other
		 * CSW failures just return the error and let the caller retry.
		 */
		return csw_rc;
	}

	if (csw.signature != CSW_SIGNATURE || csw.tag != cbw->tag) {
		/*
		 * Same rationale as above: don't send CLEAR_HALT for a bad
		 * signature; the device may simply not have finished its BOT
		 * initialisation yet, and the CLEAR_HALT would kill it.
		 */
		return -EPROTO;
	}
	if (csw.status != 0)
		return -EIO;
	if (data_rc == LIBUSB_ERROR_PIPE)
		return LIBUSB_ERROR_PIPE;
	return 0;
}

/* ------------------------------------------------------------------ */
/* High-level commands                                                */
/* ------------------------------------------------------------------ */
int rkusb_test_unit_ready(struct rkusb *u)
{
	struct cbw cbw;
	init_cbw(&cbw, RKOP_TEST_UNIT_READY, 0);
	return cbw_exec(u, &cbw, NULL, 0);
}

int rkusb_reset_device(struct rkusb *u, uint8_t subcode)
{
	struct cbw cbw;
	init_cbw(&cbw, RKOP_DEVICE_RESET, 0);
	cbw.cdb[1] = subcode;
	return cbw_exec(u, &cbw, NULL, 0);
}

int rkusb_read_flash_id(struct rkusb *u, uint8_t id[5])
{
	struct cbw cbw;
	init_cbw(&cbw, RKOP_READ_FLASH_ID, 5);
	return cbw_exec(u, &cbw, id, 5);
}

int rkusb_read_flash_info(struct rkusb *u, uint8_t info[11])
{
	struct cbw cbw;
	init_cbw(&cbw, RKOP_READ_FLASH_INFO, 11);
	return cbw_exec(u, &cbw, info, 11);
}

int rkusb_read_chip_info(struct rkusb *u, uint8_t info[16])
{
	struct cbw cbw;
	init_cbw(&cbw, RKOP_READ_CHIP_INFO, 16);
	return cbw_exec(u, &cbw, info, 16);
}

int rkusb_read_capability(struct rkusb *u, uint8_t cap[8])
{
	struct cbw cbw;
	init_cbw(&cbw, RKOP_READ_CAPABILITY, 8);
	return cbw_exec(u, &cbw, cap, 8);
}

/* CDB for LBA reads/writes:
 *   [0] opcode
 *   [1] reserved
 *   [2..5] starting sector (big-endian)
 *   [6]    reserved
 *   [7..8] sector count (big-endian)
 *   [9]    reserved
 */
static void fill_lba_cdb(struct cbw *cbw, uint32_t lba, uint16_t secs)
{
	cbw->cdb[2] = (uint8_t)(lba >> 24);
	cbw->cdb[3] = (uint8_t)(lba >> 16);
	cbw->cdb[4] = (uint8_t)(lba >> 8);
	cbw->cdb[5] = (uint8_t)lba;
	cbw->cdb[7] = (uint8_t)(secs >> 8);
	cbw->cdb[8] = (uint8_t)secs;
}

int rkusb_read_lba(struct rkusb *u, uint32_t lba, uint16_t sectors,
                   uint8_t *buf)
{
	if (sectors == 0 || sectors > RKUSB_MAX_LBA_SECTORS)
		return -EINVAL;

	struct cbw cbw;
	init_cbw(&cbw, RKOP_READ_LBA, (uint32_t)sectors * RKUSB_SECTOR_BYTES);
	fill_lba_cdb(&cbw, lba, sectors);
	return cbw_exec(u, &cbw, buf, (uint32_t)sectors * RKUSB_SECTOR_BYTES);
}

int rkusb_write_lba(struct rkusb *u, uint32_t lba, uint16_t sectors,
                    const uint8_t *buf)
{
	if (sectors == 0 || sectors > RKUSB_MAX_LBA_SECTORS)
		return -EINVAL;

	struct cbw cbw;
	init_cbw(&cbw, RKOP_WRITE_LBA, (uint32_t)sectors * RKUSB_SECTOR_BYTES);
	fill_lba_cdb(&cbw, lba, sectors);
	return cbw_exec(u, &cbw, (uint8_t *)buf,
	                (uint32_t)sectors * RKUSB_SECTOR_BYTES);
}

int rkusb_erase_lba(struct rkusb *u, uint32_t lba, uint32_t sectors)
{
	/*
	 * The ERASE_LBA CDB encodes the sector count in bytes [7:8] — a
	 * 16-bit big-endian field (max 65535 sectors per command).  Large
	 * partitions such as rootfs (99072 sectors = 0x18300) overflow this
	 * field: without splitting, only the lower 16 bits would be sent
	 * (33536 sectors = 0x8300), leaving 65536 sectors un-erased before
	 * the subsequent WriteLBA pass and silently corrupting the UBI
	 * filesystem.  Split into multiple commands of at most 0xFFFF
	 * sectors each.
	 */
	while (sectors > 0) {
		uint32_t step = sectors > 0xFFFF ? 0xFFFF : sectors;
		struct cbw cbw;
		init_cbw(&cbw, RKOP_ERASE_LBA, 0);
		cbw.cdb[2] = (uint8_t)(lba >> 24);
		cbw.cdb[3] = (uint8_t)(lba >> 16);
		cbw.cdb[4] = (uint8_t)(lba >> 8);
		cbw.cdb[5] = (uint8_t)lba;
		cbw.cdb[7] = (uint8_t)(step >> 8);
		cbw.cdb[8] = (uint8_t)step;
		int rc = cbw_exec(u, &cbw, NULL, 0);
		if (rc < 0)
			return rc;
		lba += step;
		sectors -= step;
	}
	return 0;
}

int rkusb_erase_force(struct rkusb *u, uint32_t block_start,
                      uint32_t block_count)
{
	/*
	 * ERASE_FORCE (0x0b) erases NAND flash at the native erase-block
	 * granularity.  The CDB encodes the start block number and count in
	 * the same layout as ERASE_LBA, but the unit is one NAND erase block
	 * (typically 256 sectors = 128 KB on SPI-NAND).
	 *
	 * Firmware implementations cap the per-command block count at 16
	 * (matching observed reference-tool behaviour); split larger requests
	 * accordingly.
	 */
	while (block_count > 0) {
		uint32_t step = block_count > RKUSB_ERASE_FORCE_MAX_BLOCKS
		                ? RKUSB_ERASE_FORCE_MAX_BLOCKS : block_count;
		struct cbw cbw;
		init_cbw(&cbw, RKOP_ERASE_FORCE, 0);
		cbw.cdb[2] = (uint8_t)(block_start >> 24);
		cbw.cdb[3] = (uint8_t)(block_start >> 16);
		cbw.cdb[4] = (uint8_t)(block_start >> 8);
		cbw.cdb[5] = (uint8_t)block_start;
		cbw.cdb[7] = (uint8_t)(step >> 8);
		cbw.cdb[8] = (uint8_t)step;
		int rc = cbw_exec(u, &cbw, NULL, 0);
		if (rc < 0)
			return rc;
		block_start += step;
		block_count -= step;
	}
	return 0;
}

int rkusb_write_sdram(struct rkusb *u, uint32_t addr,
                      const uint8_t *buf, uint32_t len)
{
	struct cbw cbw;
	init_cbw(&cbw, RKOP_WRITE_SDRAM, len);
	cbw.cdb[2] = (uint8_t)(addr >> 24);
	cbw.cdb[3] = (uint8_t)(addr >> 16);
	cbw.cdb[4] = (uint8_t)(addr >> 8);
	cbw.cdb[5] = (uint8_t)addr;
	cbw.cdb[7] = (uint8_t)(len >> 8);
	cbw.cdb[8] = (uint8_t)len;
	return cbw_exec(u, &cbw, (uint8_t *)buf, len);
}

int rkusb_read_sdram(struct rkusb *u, uint32_t addr, uint8_t *buf, uint32_t len)
{
	struct cbw cbw;
	init_cbw(&cbw, RKOP_READ_SDRAM, len);
	cbw.cdb[2] = (uint8_t)(addr >> 24);
	cbw.cdb[3] = (uint8_t)(addr >> 16);
	cbw.cdb[4] = (uint8_t)(addr >> 8);
	cbw.cdb[5] = (uint8_t)addr;
	cbw.cdb[7] = (uint8_t)(len >> 8);
	cbw.cdb[8] = (uint8_t)len;
	return cbw_exec(u, &cbw, buf, len);
}

int rkusb_execute_sdram(struct rkusb *u, uint32_t addr)
{
	struct cbw cbw;
	init_cbw(&cbw, RKOP_EXECUTE_SDRAM, 0);
	cbw.cdb[2] = (uint8_t)(addr >> 24);
	cbw.cdb[3] = (uint8_t)(addr >> 16);
	cbw.cdb[4] = (uint8_t)(addr >> 8);
	cbw.cdb[5] = (uint8_t)addr;
	return cbw_exec(u, &cbw, NULL, 0);
}

int rkusb_vs_read(struct rkusb *u, uint16_t index, uint8_t *buf, uint32_t len)
{
	struct cbw cbw;
	init_cbw(&cbw, RKOP_VS_READ, len);
	cbw.cdb[1] = (uint8_t)(index);
	cbw.cdb[2] = (uint8_t)(index >> 8);
	cbw.cdb[3] = (uint8_t)(len);
	cbw.cdb[4] = (uint8_t)(len >> 8);
	return cbw_exec(u, &cbw, buf, len);
}

int rkusb_vs_write(struct rkusb *u, uint16_t index, const uint8_t *buf,
                   uint32_t len)
{
	struct cbw cbw;
	init_cbw(&cbw, RKOP_VS_WRITE, len);
	cbw.cdb[1] = (uint8_t)(index);
	cbw.cdb[2] = (uint8_t)(index >> 8);
	cbw.cdb[3] = (uint8_t)(len);
	cbw.cdb[4] = (uint8_t)(len >> 8);
	return cbw_exec(u, &cbw, (uint8_t *)buf, len);
}

int rkusb_otp_read(struct rkusb *u, uint8_t *buf, uint32_t len)
{
	struct cbw cbw;
	init_cbw(&cbw, RKOP_OTP_READ, len);
	cbw.cdb[2] = (uint8_t)(len >> 8);
	cbw.cdb[3] = (uint8_t)len;
	return cbw_exec(u, &cbw, buf, len);
}

int rkusb_switch_storage(struct rkusb *u, uint8_t storage)
{
	struct cbw cbw;
	init_cbw(&cbw, RKOP_CHANGE_STORAGE, 0);
	cbw.cdb[5] = storage;
	return cbw_exec(u, &cbw, NULL, 0);
}

int rkusb_lower_format(struct rkusb *u)
{
	struct cbw cbw;
	init_cbw(&cbw, RKOP_LOWER_FORMAT, 0);
	return cbw_exec(u, &cbw, NULL, 0);
}

int rkusb_erase_blocks(struct rkusb *u, uint32_t lba, uint16_t sectors)
{
	struct cbw cbw;
	init_cbw(&cbw, RKOP_ERASE_SECTORS, 0);
	fill_lba_cdb(&cbw, lba, sectors);
	return cbw_exec(u, &cbw, NULL, 0);
}

int rkusb_write_loader_cmd(struct rkusb *u, const uint8_t *buf, uint32_t len)
{
	struct cbw cbw;
	init_cbw(&cbw, RKOP_WRITE_LOADER, len);
	cbw.cdb[2] = (uint8_t)(len >> 24);
	cbw.cdb[3] = (uint8_t)(len >> 16);
	cbw.cdb[4] = (uint8_t)(len >> 8);
	cbw.cdb[5] = (uint8_t)len;
	return cbw_exec(u, &cbw, (uint8_t *)buf, len);
}

int rkusb_raw_cmd(struct rkusb *u, uint8_t opcode, int direction,
                  const uint8_t *cdb_extra, size_t cdb_extra_len,
                  uint8_t *data, uint32_t data_len)
{
	struct cbw cbw;
	init_cbw(&cbw, opcode, data_len);
	/* Override direction and cdb length for truly exotic ops. */
	cbw.flags = direction ? 0x80 : 0x00;
	if (cdb_extra && cdb_extra_len) {
		size_t n = cdb_extra_len > 15 ? 15 : cdb_extra_len;
		memcpy(&cbw.cdb[1], cdb_extra, n);
	}
	return cbw_exec(u, &cbw, data, data_len);
}

/* ------------------------------------------------------------------ */
/* MaskROM vendor-class control-transfer loader upload                */
/* ------------------------------------------------------------------ */
int rkusb_ctrl_download(struct rkusb *u, uint16_t req_code,
                        const uint8_t *payload, size_t payload_len)
{
	if (req_code != RKUSB_CTRL_REQ_DDR_INIT &&
	    req_code != RKUSB_CTRL_REQ_LOADER)
		return -EINVAL;

	size_t total = payload_len + 2; /* CRC16 tail */
	uint8_t *buf = malloc(total);
	if (!buf)
		return -ENOMEM;

	memcpy(buf, payload, payload_len);
	uint16_t crc = rkcrc_ccitt(payload, payload_len);
	buf[payload_len]     = (uint8_t)(crc >> 8);
	buf[payload_len + 1] = (uint8_t)(crc);

	int rc = 0;
	size_t sent = 0;
	while (sent < total) {
		size_t chunk = total - sent;
		if (chunk > RKUSB_MAX_CHUNK)
			chunk = RKUSB_MAX_CHUNK;

		int n = libusb_control_transfer(
			u->handle,
			/* bmRequestType: Host->Device, Vendor, Device */
			LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			/* bRequest */      0x0c,
			/* wValue   */      0,
			/* wIndex   */      req_code,
			buf + sent, (uint16_t)chunk,
			RKUSB_CONTROL_TIMEOUT_MS);
		if (n != (int)chunk) {
			rc = n < 0 ? n : LIBUSB_ERROR_IO;
			break;
		}
		sent += chunk;
	}

	free(buf);
	return rc;
}

/* ------------------------------------------------------------------ */
/* Mode detection                                                     */
/* ------------------------------------------------------------------ */
int rkusb_probe_loader(struct rkusb *u)
{
	/*
	 * In Rockchip MaskROM the only supported bulk command is
	 * TestUnitReady (it returns success but no payload support
	 * for LBA ops).  ReadFlashInfo on MaskROM either stalls or
	 * returns CSW status=1, which surfaces as -EIO / -EPIPE.
	 *
	 * Loader mode always answers ReadFlashInfo cleanly because
	 * that's how the original tool sizes the flash before writing.
	 */
	/*
	 * Use a short timeout: if the device is still in MaskROM mode it
	 * won't respond to bulk commands at all and we don't want to block
	 * for 2 seconds per attempt while the Loader window closes.
	 * 300 ms is plenty for a responding Loader.
	 */
	unsigned saved = u->timeout_ms;
	u->timeout_ms = 300;

	int rc = rkusb_test_unit_ready(u);
	if (rc < 0) {
		clear_halt_both(u);
		u->timeout_ms = saved;
		return 0; /* definitely not a working loader */
	}

	uint8_t info[11];
	rc = rkusb_read_flash_info(u, info);
	u->timeout_ms = saved;
	if (rc < 0) {
		clear_halt_both(u);
		return 0; /* MaskROM or wedged loader */
	}
	return 1; /* loader confirmed */
}
