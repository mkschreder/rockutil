/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2024 rockutil contributors
 *
 * libusb_emu.c — libusb-1.0 emulation over a Unix domain socket.
 *
 * LD_PRELOAD this shared library to replace real libusb with a socket-backed
 * mock.  The matching Python server (usb_socket_emu.py) must be running first.
 *
 * Environment variables:
 *   ROCKUTIL_EMU_SOCKET  Path to the Unix socket (default: /tmp/rockutil-emu.sock)
 */
#define _GNU_SOURCE
#include <libusb-1.0/libusb.h>

#include <endian.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Internal definitions of opaque libusb types                        */
/* ------------------------------------------------------------------ */

struct libusb_context {
	int dummy; /* Socket is global */
};

struct libusb_device {
	int      ref_count;
	int      dev_idx;
	uint16_t vid;
	uint16_t pid;
	uint8_t  bus;
	uint8_t  addr;
	uint8_t  port_path_len;
	uint8_t  port_path[8];
	uint8_t  iManufacturer;
	uint8_t  iProduct;
};

struct libusb_device_handle {
	struct libusb_device *dev;
	uint8_t               handle_id;
};

/* ------------------------------------------------------------------ */
/* Protocol message types (must match usb_socket_emu.py)             */
/* ------------------------------------------------------------------ */
#define EMU_INIT        1
#define EMU_EXIT        2
#define EMU_DEV_LIST    3
#define EMU_GET_CONFIG  4
#define EMU_OPEN        5
#define EMU_CLOSE       6
#define EMU_CLAIM       7
#define EMU_RELEASE     8
#define EMU_BULK_OUT    9
#define EMU_BULK_IN    10
#define EMU_CTRL_OUT   11
#define EMU_CTRL_IN    12
#define EMU_CLEAR_HALT 13

/* ------------------------------------------------------------------ */
/* Global state                                                       */
/* ------------------------------------------------------------------ */
static int                   g_sock = -1;
static struct libusb_context g_ctx_singleton;

/* Device pool — allocated lazily per libusb_get_device_list call */
#define MAX_EMU_DEVICES 8
static struct libusb_device g_devpool[MAX_EMU_DEVICES];

/* ------------------------------------------------------------------ */
/* Socket I/O helpers                                                 */
/* ------------------------------------------------------------------ */

static int write_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;
	while (len > 0) {
		ssize_t n = write(fd, p, len);
		if (n <= 0)
			return -1;
		p   += n;
		len -= (size_t)n;
	}
	return 0;
}

static int read_all(int fd, void *buf, size_t len)
{
	uint8_t *p = (uint8_t *)buf;
	while (len > 0) {
		ssize_t n = read(fd, p, len);
		if (n <= 0)
			return -1;
		p   += n;
		len -= (size_t)n;
	}
	return 0;
}

/* Send request: u8 type + u32 LE payload_len + payload */
static int emu_send(uint8_t type, const void *payload, uint32_t plen)
{
	uint8_t  hdr[5];
	uint32_t le_len = htole32(plen);
	hdr[0] = type;
	memcpy(hdr + 1, &le_len, 4);
	if (write_all(g_sock, hdr, 5) < 0)
		return -1;
	if (plen > 0 && write_all(g_sock, payload, plen) < 0)
		return -1;
	return 0;
}

/* Receive response: i32 LE status + u32 LE payload_len + payload */
static int emu_recv(int32_t *status, uint8_t **out, uint32_t *out_len)
{
	int32_t  st_le;
	uint32_t len_le;
	if (read_all(g_sock, &st_le, 4) < 0)
		return -1;
	*status = (int32_t)le32toh((uint32_t)st_le);
	if (read_all(g_sock, &len_le, 4) < 0)
		return -1;
	*out_len = le32toh(len_le);
	if (*out_len > 0) {
		*out = malloc(*out_len);
		if (!*out)
			return -1;
		if (read_all(g_sock, *out, *out_len) < 0) {
			free(*out);
			*out = NULL;
			return -1;
		}
	} else {
		*out = NULL;
	}
	return 0;
}

/* Send a simple message and return status (frees payload). */
static int32_t emu_rpc0(uint8_t type, const void *payload, uint32_t plen)
{
	if (g_sock < 0)
		return LIBUSB_ERROR_NO_DEVICE;
	if (emu_send(type, payload, plen) < 0)
		return LIBUSB_ERROR_IO;
	int32_t  status;
	uint8_t *resp = NULL;
	uint32_t resp_len = 0;
	if (emu_recv(&status, &resp, &resp_len) < 0) {
		free(resp);
		return LIBUSB_ERROR_IO;
	}
	free(resp);
	return status;
}

/* ------------------------------------------------------------------ */
/* Socket connection                                                  */
/* ------------------------------------------------------------------ */
static int emu_connect(void)
{
	const char *path = getenv("ROCKUTIL_EMU_SOCKET");
	if (!path)
		path = "/tmp/rockutil-emu.sock";

	g_sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (g_sock < 0)
		return -1;

	struct sockaddr_un sa;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);

	if (connect(g_sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(g_sock);
		g_sock = -1;
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* libusb_init / libusb_exit                                          */
/* ------------------------------------------------------------------ */
int libusb_init(libusb_context **ctx)
{
	if (g_sock < 0) {
		/* Retry connection a few times — emulator may still be starting */
		for (int i = 0; i < 50; ++i) {
			if (emu_connect() == 0)
				break;
			usleep(100 * 1000); /* 100 ms */
		}
		if (g_sock < 0) {
			fprintf(stderr, "[libusb_emu] Cannot connect to emulator socket\n");
			return LIBUSB_ERROR_NO_DEVICE;
		}
	}
	int32_t st = emu_rpc0(EMU_INIT, NULL, 0);
	if (ctx)
		*ctx = &g_ctx_singleton;
	return st < 0 ? (int)st : LIBUSB_SUCCESS;
}

/* libusb >= 1.0.27 may call libusb_init_context — map it to libusb_init */
int libusb_init_context(libusb_context **ctx, const struct libusb_init_option *options,
                        int num_options)
{
	(void)options;
	(void)num_options;
	return libusb_init(ctx);
}

void libusb_exit(libusb_context *ctx)
{
	(void)ctx;
	if (g_sock < 0)
		return;
	emu_rpc0(EMU_EXIT, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* libusb_set_option (no-op)                                          */
/* ------------------------------------------------------------------ */
int libusb_set_option(libusb_context *ctx, enum libusb_option option, ...)
{
	(void)ctx;
	(void)option;
	return LIBUSB_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Error strings                                                      */
/* ------------------------------------------------------------------ */
const char *libusb_error_name(int errcode)
{
	switch (errcode) {
	case LIBUSB_SUCCESS:             return "LIBUSB_SUCCESS";
	case LIBUSB_ERROR_IO:            return "LIBUSB_ERROR_IO";
	case LIBUSB_ERROR_INVALID_PARAM: return "LIBUSB_ERROR_INVALID_PARAM";
	case LIBUSB_ERROR_ACCESS:        return "LIBUSB_ERROR_ACCESS";
	case LIBUSB_ERROR_NO_DEVICE:     return "LIBUSB_ERROR_NO_DEVICE";
	case LIBUSB_ERROR_NOT_FOUND:     return "LIBUSB_ERROR_NOT_FOUND";
	case LIBUSB_ERROR_BUSY:          return "LIBUSB_ERROR_BUSY";
	case LIBUSB_ERROR_TIMEOUT:       return "LIBUSB_ERROR_TIMEOUT";
	case LIBUSB_ERROR_OVERFLOW:      return "LIBUSB_ERROR_OVERFLOW";
	case LIBUSB_ERROR_PIPE:          return "LIBUSB_ERROR_PIPE";
	case LIBUSB_ERROR_INTERRUPTED:   return "LIBUSB_ERROR_INTERRUPTED";
	case LIBUSB_ERROR_NO_MEM:        return "LIBUSB_ERROR_NO_MEM";
	case LIBUSB_ERROR_NOT_SUPPORTED: return "LIBUSB_ERROR_NOT_SUPPORTED";
	case LIBUSB_ERROR_OTHER:         return "LIBUSB_ERROR_OTHER";
	default:                         return "LIBUSB_ERROR_UNKNOWN";
	}
}

const char *libusb_strerror(int errcode)
{
	return libusb_error_name(errcode);
}

/* ------------------------------------------------------------------ */
/* Device enumeration                                                 */
/* ------------------------------------------------------------------ */

/*
 * DEV_LIST response payload per device (16 bytes):
 *   u16 vid, u16 pid, u8 bus, u8 addr, u8 port_path_len,
 *   u8 ports[7], u8 iManufacturer, u8 iProduct
 */
#define DEV_INFO_SIZE 16

ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list)
{
	(void)ctx;
	if (g_sock < 0) {
		*list = NULL;
		return LIBUSB_ERROR_NO_DEVICE;
	}

	if (emu_send(EMU_DEV_LIST, NULL, 0) < 0)
		return LIBUSB_ERROR_IO;

	int32_t  status;
	uint8_t *resp     = NULL;
	uint32_t resp_len = 0;
	if (emu_recv(&status, &resp, &resp_len) < 0)
		return LIBUSB_ERROR_IO;
	if (status < 0) {
		free(resp);
		return status;
	}

	uint8_t count = resp ? resp[0] : 0;
	if (count > MAX_EMU_DEVICES)
		count = MAX_EMU_DEVICES;

	/* Allocate the device pointer array (NULL-terminated) */
	libusb_device **arr = calloc(count + 1, sizeof(*arr));
	if (!arr) {
		free(resp);
		return LIBUSB_ERROR_NO_MEM;
	}

	for (uint8_t i = 0; i < count; ++i) {
		const uint8_t *p = resp + 1 + i * DEV_INFO_SIZE;
		struct libusb_device *d = &g_devpool[i];
		memset(d, 0, sizeof(*d));
		d->ref_count = 1;
		d->dev_idx   = i;
		memcpy(&d->vid, p + 0, 2); d->vid = le16toh(d->vid);
		memcpy(&d->pid, p + 2, 2); d->pid = le16toh(d->pid);
		d->bus              = p[4];
		d->addr             = p[5];
		d->port_path_len    = p[6];
		memcpy(d->port_path, p + 7, 7);
		d->iManufacturer    = p[14];
		d->iProduct         = p[15];
		arr[i] = d;
	}
	arr[count] = NULL;
	free(resp);
	*list = arr;
	return count;
}

void libusb_free_device_list(libusb_device **list, int unref_devices)
{
	if (!list)
		return;
	if (unref_devices) {
		for (int i = 0; list[i]; ++i)
			libusb_unref_device(list[i]);
	}
	free(list);
}

libusb_device *libusb_ref_device(libusb_device *dev)
{
	if (dev)
		dev->ref_count++;
	return dev;
}

void libusb_unref_device(libusb_device *dev)
{
	if (!dev)
		return;
	dev->ref_count--;
	/* Devices live in g_devpool — not individually freed */
}

int libusb_get_device_descriptor(libusb_device *dev,
                                  struct libusb_device_descriptor *desc)
{
	if (!dev || !desc)
		return LIBUSB_ERROR_INVALID_PARAM;
	memset(desc, 0, sizeof(*desc));
	desc->bLength         = 18;
	desc->bDescriptorType = LIBUSB_DT_DEVICE;
	desc->bcdUSB          = 0x0200;
	desc->idVendor        = dev->vid;
	desc->idProduct       = dev->pid;
	desc->bcdDevice       = 0x0100;
	desc->iManufacturer   = dev->iManufacturer;
	desc->iProduct        = dev->iProduct;
	desc->bNumConfigurations = 1;
	return LIBUSB_SUCCESS;
}

uint8_t libusb_get_bus_number(libusb_device *dev)
{
	return dev ? dev->bus : 0;
}

uint8_t libusb_get_device_address(libusb_device *dev)
{
	return dev ? dev->addr : 0;
}

int libusb_get_port_numbers(libusb_device *dev, uint8_t *port_numbers,
                             int port_numbers_len)
{
	if (!dev || !port_numbers || port_numbers_len <= 0)
		return LIBUSB_ERROR_INVALID_PARAM;
	int n = dev->port_path_len;
	if (n > port_numbers_len)
		n = port_numbers_len;
	memcpy(port_numbers, dev->port_path, (size_t)n);
	return n;
}

/* ------------------------------------------------------------------ */
/* Config descriptor                                                  */
/* ------------------------------------------------------------------ */

/*
 * GET_CONFIG response payload:
 *   u8 bNumInterfaces
 *   For each interface:
 *     u8 ifnum, u8 class, u8 subclass, u8 protocol, u8 num_eps
 *     For each ep: u8 addr, u8 attrs, u16 maxpkt (LE)
 */
int libusb_get_active_config_descriptor(libusb_device *dev,
                                         struct libusb_config_descriptor **config)
{
	if (!dev || !config)
		return LIBUSB_ERROR_INVALID_PARAM;
	if (g_sock < 0)
		return LIBUSB_ERROR_NO_DEVICE;

	uint8_t payload = (uint8_t)dev->dev_idx;
	if (emu_send(EMU_GET_CONFIG, &payload, 1) < 0)
		return LIBUSB_ERROR_IO;

	int32_t  status;
	uint8_t *resp     = NULL;
	uint32_t resp_len = 0;
	if (emu_recv(&status, &resp, &resp_len) < 0)
		return LIBUSB_ERROR_IO;
	if (status < 0) {
		free(resp);
		return (int)status;
	}

	/* Parse the response into proper libusb descriptor structs.
	 * We use a flat allocation so free() on the config descriptor is safe. */
	uint8_t n_ifaces = resp ? resp[0] : 0;
	if (n_ifaces == 0) {
		free(resp);
		return LIBUSB_ERROR_NOT_FOUND;
	}

	/* Count total endpoints across all interfaces */
	int total_eps = 0;
	{
		const uint8_t *p = resp + 1;
		for (uint8_t i = 0; i < n_ifaces; ++i) {
			if ((size_t)(p - resp) + 5 > resp_len)
				break;
			uint8_t ne = p[4];
			total_eps += ne;
			p += 5 + ne * 4;
		}
	}

	/* Flat allocation: config + ifaces + idescs + eps */
	size_t sz = sizeof(struct libusb_config_descriptor) +
	            (size_t)n_ifaces * sizeof(struct libusb_interface) +
	            (size_t)n_ifaces * sizeof(struct libusb_interface_descriptor) +
	            (size_t)total_eps * sizeof(struct libusb_endpoint_descriptor);
	uint8_t *blk = calloc(1, sz);
	if (!blk) {
		free(resp);
		return LIBUSB_ERROR_NO_MEM;
	}

	struct libusb_config_descriptor    *cfg   = (void *)(blk);
	struct libusb_interface            *ifarr = (void *)(blk + sizeof(*cfg));
	struct libusb_interface_descriptor *idarr =
		(void *)((uint8_t *)ifarr + (size_t)n_ifaces * sizeof(*ifarr));
	struct libusb_endpoint_descriptor  *eparr =
		(void *)((uint8_t *)idarr + (size_t)n_ifaces * sizeof(*idarr));

	cfg->bLength         = 9;
	cfg->bDescriptorType = LIBUSB_DT_CONFIG;
	cfg->bNumInterfaces  = n_ifaces;
	/* Cast away const — we own the memory */
	*(const struct libusb_interface **)&cfg->interface = ifarr;

	const uint8_t *p = resp + 1;
	int ep_off = 0;
	for (uint8_t i = 0; i < n_ifaces; ++i) {
		uint8_t ifnum   = p[0];
		uint8_t cls     = p[1];
		uint8_t subcls  = p[2];
		uint8_t proto   = p[3];
		uint8_t num_eps = p[4];
		p += 5;

		ifarr[i].num_altsetting = 1;
		*(const struct libusb_interface_descriptor **)&ifarr[i].altsetting =
			&idarr[i];

		idarr[i].bLength            = 9;
		idarr[i].bDescriptorType    = LIBUSB_DT_INTERFACE;
		idarr[i].bInterfaceNumber   = ifnum;
		idarr[i].bAlternateSetting  = 0;
		idarr[i].bNumEndpoints      = num_eps;
		idarr[i].bInterfaceClass    = cls;
		idarr[i].bInterfaceSubClass = subcls;
		idarr[i].bInterfaceProtocol = proto;
		*(const struct libusb_endpoint_descriptor **)&idarr[i].endpoint =
			&eparr[ep_off];

		for (uint8_t e = 0; e < num_eps; ++e) {
			uint16_t maxpkt;
			memcpy(&maxpkt, p + 2, 2);
			maxpkt = le16toh(maxpkt);
			eparr[ep_off + e].bLength          = 7;
			eparr[ep_off + e].bDescriptorType  = LIBUSB_DT_ENDPOINT;
			eparr[ep_off + e].bEndpointAddress = p[0];
			eparr[ep_off + e].bmAttributes     = p[1];
			eparr[ep_off + e].wMaxPacketSize    = maxpkt;
			p += 4;
		}
		ep_off += num_eps;
	}

	free(resp);
	*config = cfg;
	return LIBUSB_SUCCESS;
}

void libusb_free_config_descriptor(struct libusb_config_descriptor *config)
{
	/* Our config is a flat malloc — free() is safe */
	free(config);
}

/* ------------------------------------------------------------------ */
/* Open / close                                                       */
/* ------------------------------------------------------------------ */
int libusb_open(libusb_device *dev, libusb_device_handle **handle)
{
	if (!dev || !handle)
		return LIBUSB_ERROR_INVALID_PARAM;
	if (g_sock < 0)
		return LIBUSB_ERROR_NO_DEVICE;

	uint8_t payload = (uint8_t)dev->dev_idx;
	if (emu_send(EMU_OPEN, &payload, 1) < 0)
		return LIBUSB_ERROR_IO;

	int32_t  status;
	uint8_t *resp     = NULL;
	uint32_t resp_len = 0;
	if (emu_recv(&status, &resp, &resp_len) < 0)
		return LIBUSB_ERROR_IO;
	if (status < 0) {
		free(resp);
		return (int)status;
	}

	uint8_t hid = resp ? resp[0] : 0;
	free(resp);

	libusb_device_handle *h = calloc(1, sizeof(*h));
	if (!h)
		return LIBUSB_ERROR_NO_MEM;
	h->dev       = dev;
	h->handle_id = hid;
	libusb_ref_device(dev);
	*handle = h;
	return LIBUSB_SUCCESS;
}

void libusb_close(libusb_device_handle *handle)
{
	if (!handle)
		return;
	if (g_sock >= 0) {
		uint8_t payload = handle->handle_id;
		emu_rpc0(EMU_CLOSE, &payload, 1);
	}
	libusb_unref_device(handle->dev);
	free(handle);
}

/* ------------------------------------------------------------------ */
/* Interface claim / release                                          */
/* ------------------------------------------------------------------ */
int libusb_claim_interface(libusb_device_handle *handle, int interface_number)
{
	if (!handle)
		return LIBUSB_ERROR_INVALID_PARAM;
	uint8_t payload[2] = { handle->handle_id, (uint8_t)interface_number };
	return (int)emu_rpc0(EMU_CLAIM, payload, 2);
}

int libusb_release_interface(libusb_device_handle *handle, int interface_number)
{
	if (!handle)
		return LIBUSB_ERROR_INVALID_PARAM;
	uint8_t payload[2] = { handle->handle_id, (uint8_t)interface_number };
	return (int)emu_rpc0(EMU_RELEASE, payload, 2);
}

/* ------------------------------------------------------------------ */
/* Bulk transfer                                                      */
/* ------------------------------------------------------------------ */
int libusb_bulk_transfer(libusb_device_handle *handle, unsigned char endpoint,
                          unsigned char *data, int length,
                          int *transferred, unsigned int timeout)
{
	(void)timeout;
	if (!handle || !data || length < 0)
		return LIBUSB_ERROR_INVALID_PARAM;
	if (g_sock < 0)
		return LIBUSB_ERROR_NO_DEVICE;

	if (endpoint & LIBUSB_ENDPOINT_IN) {
		/* Bulk IN: request data from emulator */
		uint8_t hdr[6];
		hdr[0] = handle->handle_id;
		hdr[1] = endpoint;
		uint32_t ml = htole32((uint32_t)length);
		memcpy(hdr + 2, &ml, 4);
		if (emu_send(EMU_BULK_IN, hdr, 6) < 0)
			return LIBUSB_ERROR_IO;

		int32_t  status;
		uint8_t *resp     = NULL;
		uint32_t resp_len = 0;
		if (emu_recv(&status, &resp, &resp_len) < 0)
			return LIBUSB_ERROR_IO;
		if (status < 0) {
			free(resp);
			return (int)status;
		}
		/* First 4 bytes of payload = u32 LE actual length */
		uint32_t actual = 0;
		if (resp && resp_len >= 4)
			memcpy(&actual, resp, 4);
		actual = le32toh(actual);
		if (actual > (uint32_t)length)
			actual = (uint32_t)length;
		if (resp && resp_len > 4)
			memcpy(data, resp + 4, actual);
		free(resp);
		if (transferred)
			*transferred = (int)actual;
		return LIBUSB_SUCCESS;
	} else {
		/* Bulk OUT: send data to emulator */
		uint32_t plen = 6 + (uint32_t)length;
		uint8_t *buf = malloc(plen);
		if (!buf)
			return LIBUSB_ERROR_NO_MEM;
		buf[0] = handle->handle_id;
		buf[1] = endpoint;
		uint32_t dl = htole32((uint32_t)length);
		memcpy(buf + 2, &dl, 4);
		memcpy(buf + 6, data, (size_t)length);
		if (emu_send(EMU_BULK_OUT, buf, plen) < 0) {
			free(buf);
			return LIBUSB_ERROR_IO;
		}
		free(buf);

		int32_t  status;
		uint8_t *resp     = NULL;
		uint32_t resp_len = 0;
		if (emu_recv(&status, &resp, &resp_len) < 0)
			return LIBUSB_ERROR_IO;
		if (status < 0) {
			free(resp);
			return (int)status;
		}
		uint32_t actual = 0;
		if (resp && resp_len >= 4)
			memcpy(&actual, resp, 4);
		actual = le32toh(actual);
		free(resp);
		if (transferred)
			*transferred = (int)actual;
		return LIBUSB_SUCCESS;
	}
}

/* ------------------------------------------------------------------ */
/* Control transfer                                                   */
/* ------------------------------------------------------------------ */
int libusb_control_transfer(libusb_device_handle *handle,
                             uint8_t request_type, uint8_t bRequest,
                             uint16_t wValue, uint16_t wIndex,
                             unsigned char *data, uint16_t wLength,
                             unsigned int timeout)
{
	(void)timeout;
	if (!handle)
		return LIBUSB_ERROR_INVALID_PARAM;
	if (g_sock < 0)
		return LIBUSB_ERROR_NO_DEVICE;

	int is_in = (request_type & LIBUSB_ENDPOINT_IN) != 0;

	if (is_in) {
		/* Control IN */
		uint8_t hdr[9];
		hdr[0] = handle->handle_id;
		hdr[1] = request_type;
		hdr[2] = bRequest;
		uint16_t v = htole16(wValue);
		uint16_t i = htole16(wIndex);
		uint16_t l = htole16(wLength);
		memcpy(hdr + 3, &v, 2);
		memcpy(hdr + 5, &i, 2);
		memcpy(hdr + 7, &l, 2);
		if (emu_send(EMU_CTRL_IN, hdr, 9) < 0)
			return LIBUSB_ERROR_IO;

		int32_t  status;
		uint8_t *resp     = NULL;
		uint32_t resp_len = 0;
		if (emu_recv(&status, &resp, &resp_len) < 0)
			return LIBUSB_ERROR_IO;
		if (status < 0) {
			free(resp);
			return (int)status;
		}
		uint32_t actual = 0;
		if (resp && resp_len >= 4)
			memcpy(&actual, resp, 4);
		actual = le32toh(actual);
		if (actual > wLength)
			actual = wLength;
		if (resp && resp_len > 4 && data)
			memcpy(data, resp + 4, actual);
		free(resp);
		return (int)actual;
	} else {
		/* Control OUT */
		uint32_t plen = 9 + wLength;
		uint8_t *buf = malloc(plen);
		if (!buf)
			return LIBUSB_ERROR_NO_MEM;
		buf[0] = handle->handle_id;
		buf[1] = request_type;
		buf[2] = bRequest;
		uint16_t v = htole16(wValue);
		uint16_t ii = htole16(wIndex);
		uint16_t l  = htole16(wLength);
		memcpy(buf + 3, &v, 2);
		memcpy(buf + 5, &ii, 2);
		memcpy(buf + 7, &l, 2);
		if (data && wLength > 0)
			memcpy(buf + 9, data, wLength);
		if (emu_send(EMU_CTRL_OUT, buf, plen) < 0) {
			free(buf);
			return LIBUSB_ERROR_IO;
		}
		free(buf);

		int32_t  status;
		uint8_t *resp     = NULL;
		uint32_t resp_len = 0;
		if (emu_recv(&status, &resp, &resp_len) < 0)
			return LIBUSB_ERROR_IO;
		if (status < 0) {
			free(resp);
			return (int)status;
		}
		uint32_t actual = 0;
		if (resp && resp_len >= 4)
			memcpy(&actual, resp, 4);
		actual = le32toh(actual);
		free(resp);
		return (int)actual;
	}
}

/* ------------------------------------------------------------------ */
/* Misc                                                               */
/* ------------------------------------------------------------------ */
int libusb_clear_halt(libusb_device_handle *handle, unsigned char endpoint)
{
	if (!handle)
		return LIBUSB_ERROR_INVALID_PARAM;
	uint8_t payload[2] = { handle->handle_id, endpoint };
	return (int)emu_rpc0(EMU_CLEAR_HALT, payload, 2);
}

int libusb_kernel_driver_active(libusb_device_handle *handle, int interface_number)
{
	(void)handle;
	(void)interface_number;
	return 0; /* No kernel driver attached in emulated environment */
}

int libusb_detach_kernel_driver(libusb_device_handle *handle, int interface_number)
{
	(void)handle;
	(void)interface_number;
	return LIBUSB_SUCCESS;
}

libusb_device *libusb_get_device(libusb_device_handle *handle)
{
	return handle ? handle->dev : NULL;
}
