/***************************************************************************
 *   Copyright (C) 2016-2017 by Nuvoton                                    *
 *   Zale Yu <cyyu@nuvoton.com>                                            *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* project specific includes */
#include <helper/binarybuffer.h>
#include <jtag/adapter.h>
#include <jtag/interface.h>
#include <jtag/hla/hla_layout.h>
#include <jtag/hla/hla_transport.h>
#include <jtag/hla/hla_interface.h>
#include <target/target.h>

#include <target/cortex_m.h>
#include <helper/log.h>
#include "libusb_helper.h"
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#define ENDPOINT_IN  0x80
#define ENDPOINT_OUT 0x00

#define NULINK_WRITE_TIMEOUT 1000
#define NULINK_READ_TIMEOUT  1000


#define NULINK_INTERFACE_NUM  0
#define NULINK2_INTERFACE_NUM 3
#define NULINK_RX_EP  (1|ENDPOINT_IN)
#define NULINK_TX_EP  (2|ENDPOINT_OUT)
#define NULINK2_RX_EP (6|ENDPOINT_IN)
#define NULINK2_TX_EP (7|ENDPOINT_OUT)
#define NULINK_HID_MAX_SIZE   (64)
#define NULINK2_HID_MAX_SIZE   (1024)
#define V6M_MAX_COMMAND_LENGTH (NULINK_HID_MAX_SIZE - 2)
#define V7M_MAX_COMMAND_LENGTH (NULINK2_HID_MAX_SIZE - 3)

#define USBCMD_TIMEOUT		5000

#define NULINK2_USB_PID1  (0x5200)
#define NULINK2_USB_PID2  (0x5201)

#define NULINK_SERIAL_LEN	(32)

struct nulink_usb_handle_s {
	struct libusb_device_handle *fd;
	struct libusb_transfer *trans;
	uint8_t interface_num;
	uint8_t rx_ep;
	uint8_t tx_ep;
	uint16_t max_packet_size;
	uint32_t usbcmdidx;
	uint8_t cmdidx;
	uint8_t cmdsize;
	uint8_t cmdbuf[NULINK2_HID_MAX_SIZE];
	uint8_t tempbuf[NULINK2_HID_MAX_SIZE];
	uint8_t databuf[NULINK2_HID_MAX_SIZE];
	uint32_t max_mem_packet;
	enum hl_transports transport;
	uint16_t hardwareConfig; /* bit 0: 1:Nu-Link-Pro, 0:Normal Nu-Link | bit 1: 1:Nu-Link2, 0:Nu-Link */
	uint32_t reset_command;
	uint32_t extMode_command;
	uint32_t io_voltage;
	uint32_t chip_type;
} *m_nulink_usb_handle;

struct nulink_usb_internal_api_s {
	int (*nulink_usb_xfer) (void *handle, uint8_t *buf, int size);
	void (*nulink_usb_init_buffer) (void *handle, uint32_t size);
} m_nulink_usb_api;

/* ICE Command */
#define CMD_READ_REG				0xB5UL
#define CMD_READ_RAM				0xB1UL
#define CMD_WRITE_REG				0xB8UL
#define CMD_WRITE_RAM				0xB9UL
#define CMD_CHECK_ID				0xA3UL
#define CMD_MCU_RESET				0xE2UL
#define CMD_CHECK_MCU_STOP			0xD8UL
#define CMD_MCU_STEP_RUN			0xD1UL
#define CMD_MCU_STOP_RUN			0xD2UL
#define CMD_MCU_FREE_RUN			0xD3UL
#define CMD_SET_CONFIG				0xA2UL
#define CMD_ERASE_FLASHCHIP			0xA4UL
#define ARM_SRAM_BASE				0x20000000UL

#define HARDWARE_CONFIG_NULINKPRO	1
#define HARDWARE_CONFIG_NULINK2		2

enum PROCESSOR_STATE_E {
	PROCESSOR_STOP,
	PROCESSOR_RUN,
	PROCESSOR_IDLE,
	PROCESSOR_POWER_DOWN
};

enum RESET_E
{
		RESET_AUTO			= 0,
		RESET_HW			= 1,
		RESET_SYSRESETREQ	= 2,
		RESET_VECTRESET		= 3,
		RESET_FAST_RESCUE	= 4,	/* Rescue and erase the chip, need very fast speed */
		RESET_NONE_NULINK	= 5,	/* Connect only */
		RESET_NONE2			= 6		/* For 8051 1T */
	};

enum CONNECT_E {
	CONNECT_NORMAL = 0,      /* Support all reset method */
	CONNECT_PRE_RESET = 1,   /* Support all reset method */
	CONNECT_UNDER_RESET = 2, /* Support all reset method */
	CONNECT_NONE = 3,        /* Support RESET_HW, (RESET_AUTO = RESET_HW) */
	CONNECT_DISCONNECT = 4,  /* Support RESET_NONE, (RESET_AUTO = RESET_NONE) */
	CONNECT_ICP_MODE = 5     /* Support NUC505 ICP mode*/
};

enum EXTMODE_E {
	EXTMODE_NORMAL = 0,        /* Support the most of Nuvoton chips */
	EXTMODE_M0A21  = 0x100,    /* Support M0A21 */
	EXTMODE_M030G  = 0x10000 , /* Support M030G */
};

enum NUC_CHIP_TYPE_E {
	NUC_CHIP_TYPE_M460	= 0x49A,
};

static void print64BytesBufferContent(char *bufferName, uint8_t *buf, int size)
{
	unsigned i, j;
	LOG_DEBUG("%s:", bufferName);

	for (i = 0; i < 4; i++) {
		j = i * 16;
		LOG_DEBUG("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x ",
		buf[j + 0],  buf[j + 1],  buf[j + 2],  buf[j + 3],
		buf[j + 4],  buf[j + 5],  buf[j + 6],  buf[j + 7],
		buf[j + 8],  buf[j + 9],  buf[j + 10], buf[j + 11],
		buf[j + 12], buf[j + 13], buf[j + 14], buf[j + 15]
		);
	}
}

static uint16_t jtag_libusb_get_maxPacketSize(struct libusb_device_handle *devh, uint8_t configuration,
		uint8_t interface_num, uint8_t *usb_read_ep, uint8_t *usb_write_ep)
{
	struct libusb_device *udev = libusb_get_device(devh);
	struct libusb_config_descriptor *config = NULL;
	int retCode = -99;
	uint16_t result = -1;
	*usb_read_ep = *usb_write_ep = 0;

	retCode = libusb_get_config_descriptor(udev, configuration, &config);
	if (retCode != 0 || config == NULL)
	{
		LOG_ERROR("libusb_get_config_descriptor() failed with %s", libusb_error_name(retCode));
		return result;
	}

	const struct libusb_interface_descriptor *descriptor;
	descriptor = &config->interface[interface_num].altsetting[0];

	for (int i = 0; i < descriptor->bNumEndpoints; i++) {
		if (descriptor->endpoint[i].bEndpointAddress & 0x80) {
			result = descriptor->endpoint[i].wMaxPacketSize;
			break;
		}
	}

	for (int i = 0; i < descriptor->bNumEndpoints; i++) {
		//input
		if (descriptor->endpoint[i].bEndpointAddress & 0x80) {
			*usb_read_ep = descriptor->endpoint[i].bEndpointAddress;
		}//output
		else {
			*usb_write_ep = descriptor->endpoint[i].bEndpointAddress;
		}

		if (*usb_read_ep && *usb_write_ep) {
			break;
		}
	}

	libusb_free_config_descriptor(config);

	return result;
}

#ifndef _WIN32
double GetTickCount(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return 0;
	return now.tv_sec * 1000.0 + now.tv_nsec / 1000000.0;
}
#endif

static void nulink1_usb_init_buffer(void *handle, uint32_t size);

static int nulink1_usb_xfer_rw(void *handle, int cmdsize, uint8_t *buf)
{
	struct nulink_usb_handle_s *h = handle;
	int res = ERROR_OK, startTime = GetTickCount(), cmdID;
	int transferred;
	assert(handle != NULL);
#if defined(_WIN32) && (NUVOTON_CUSTOMIZED)
	jtag_libusb_nuvoton_mutex_lock();
#endif
	jtag_libusb_interrupt_write(h->fd, h->tx_ep, (char *)h->cmdbuf, h->max_packet_size,
		NULINK_WRITE_TIMEOUT, &transferred);
	#if 0
	if (debug_level >= LOG_LVL_DEBUG)
	{
		char bufName[20] = "cmd transferred";
		print64BytesBufferContent(bufName, h->cmdbuf, h->max_packet_size);
	}
	#endif
	do {
		jtag_libusb_interrupt_read(h->fd, h->rx_ep, (char *)buf,
			h->max_packet_size, NULINK_READ_TIMEOUT, &transferred);
		#if 0
		if (debug_level >= LOG_LVL_DEBUG)
		{
			char bufName1[20] = "data received";
			print64BytesBufferContent(bufName1, buf, h->max_packet_size);
		}
		#endif
		if(GetTickCount() - startTime > USBCMD_TIMEOUT)
		{
			res = ERROR_FAIL;
			break;
		}
		cmdID = h->cmdbuf[2];
	} while ((h->cmdbuf[0] != (buf[0] & 0x7F)) ||
			(cmdsize != buf[1]) ||
			(cmdID != 0xff && cmdID != CMD_WRITE_REG && cmdID != CMD_WRITE_RAM &&
			 cmdID != CMD_CHECK_MCU_STOP  && cmdID != buf[2]));
#if defined(_WIN32) && (NUVOTON_CUSTOMIZED)
	jtag_libusb_nuvoton_mutex_unlock();
#endif
	return res;
}

static int nulink2_usb_xfer_rw(void *handle, int cmdsize, uint8_t *buf)
{
	struct nulink_usb_handle_s *h = handle;
	int res = ERROR_OK, startTime = GetTickCount(), cmdID;
	int transferred;
	assert(handle != NULL);
#if defined(_WIN32) && (NUVOTON_CUSTOMIZED)
	jtag_libusb_nuvoton_mutex_lock();
#endif
	jtag_libusb_interrupt_write(h->fd, h->tx_ep, (char *)h->cmdbuf, h->max_packet_size,
		NULINK_WRITE_TIMEOUT, &transferred);
	#if 0
	if (debug_level >= LOG_LVL_DEBUG)
	{
		char bufName[20] = "cmd transferred";
		print64BytesBufferContent(bufName, h->cmdbuf, h->max_packet_size);
	}
	#endif
	do {
		jtag_libusb_interrupt_read(h->fd, h->rx_ep, (char *)buf,
			h->max_packet_size, NULINK_READ_TIMEOUT, &transferred);
		#if 0
		if (debug_level >= LOG_LVL_DEBUG)
		{
			char bufName1[20] = "data received";
			print64BytesBufferContent(bufName1, buf, h->max_packet_size);
		}
		#endif
		if(GetTickCount() - startTime > USBCMD_TIMEOUT)
		{
			res = ERROR_FAIL;
			break;
		}
		cmdID = h->cmdbuf[3];
	} while ((h->cmdbuf[0] != (buf[0] & 0x7F)) ||
			(cmdsize != (((int)buf[1]) << 8) + ((int)buf[2] & 0xFF)) ||
			(cmdID != 0xff && cmdID != CMD_WRITE_REG && cmdID != CMD_WRITE_RAM &&
			 cmdID != CMD_CHECK_MCU_STOP && cmdID != buf[3]));
#if defined(_WIN32) && (NUVOTON_CUSTOMIZED)
	jtag_libusb_nuvoton_mutex_unlock();
#endif
	return res;
}

static int nulink1_usb_xfer(void *handle, uint8_t *buf, int size)
{
	struct nulink_usb_handle_s *h = handle;

	assert(handle != NULL);

	int err =  nulink1_usb_xfer_rw(h, size, h->tempbuf);

	memcpy(buf, h->tempbuf + 2, h->max_packet_size - 2);

	return err;
}

static int nulink2_usb_xfer(void *handle, uint8_t *buf, int size)
{
	struct nulink_usb_handle_s *h = handle;

	assert(handle);

	int err = nulink2_usb_xfer_rw(h, size, h->tempbuf);

	memcpy(buf, h->tempbuf + 3, h->max_packet_size - 3);

	return err;
}

static void nulink1_usb_init_buffer(void *handle, uint32_t size)
{
	struct nulink_usb_handle_s *h = handle;

	h->cmdidx = 0;

	memset(h->cmdbuf, 0, h->max_packet_size);
	memset(h->tempbuf, 0, h->max_packet_size);
	memset(h->databuf, 0, h->max_packet_size);

	h->cmdbuf[0] = (char)(++h->usbcmdidx & (unsigned char)0x7F);
	h->cmdbuf[1] = (char)size;
	h->cmdidx += 2;

}

static void nulink2_usb_init_buffer(void *handle, uint32_t size)
{
	struct nulink_usb_handle_s *h = handle;

	h->cmdidx = 0;

	memset(h->cmdbuf, 0, h->max_packet_size);
	memset(h->tempbuf, 0, h->max_packet_size);
	memset(h->databuf, 0, h->max_packet_size);

	h->cmdbuf[0] = (char)(++h->usbcmdidx & (unsigned char)0x7F);
	h_u16_to_le(h->cmdbuf + 1, size);
	h->cmdidx += 3;
}

static int nulink_usb_version(void *handle)
{
	struct nulink_usb_handle_s *h = handle;

	LOG_DEBUG("nulink_usb_version");

	assert(handle);

	m_nulink_usb_api.nulink_usb_init_buffer(handle, V6M_MAX_COMMAND_LENGTH);

	memset(h->cmdbuf + h->cmdidx, 0xFF, V6M_MAX_COMMAND_LENGTH);
	h->cmdbuf[h->cmdidx + 4] = 0xA1; /* host_rev_num: 6561 */;
	h->cmdbuf[h->cmdidx + 5] = 0x19;

	int res = m_nulink_usb_api.nulink_usb_xfer(handle, h->databuf, h->cmdsize);

	if (res != ERROR_OK)
		return res;

	LOG_INFO("Nu-Link firmware_version %" PRIu32 ", product_id (0x%08" PRIx32 ")",
			 le_to_h_u32(h->databuf),
			 le_to_h_u32(h->databuf + 4 * 1));

	const bool is_nulinkpro = !!(le_to_h_u32(h->databuf + 4 * 2) & 1);
	if (is_nulinkpro) {
		LOG_INFO("Adapter is Nu-Link-Pro, target_voltage_mv(%" PRIu16 "), usb_voltage_mv(%" PRIu16 ")",
				 le_to_h_u16(h->databuf + 4 * 3 + 0),
				 le_to_h_u16(h->databuf + 4 * 3 + 2));

		h->hardwareConfig |= HARDWARE_CONFIG_NULINKPRO;
	} else {
		LOG_INFO("Adapter is Nu-Link");
	}

	return ERROR_OK;
}

static int nulink_usb_assert_srst(void *handle, int srst);
static int nulink_usb_idcode(void *handle, uint32_t *idcode)
{
	struct nulink_usb_handle_s *h = handle;

	LOG_DEBUG("nulink_usb_idcode");

	assert(handle);

	m_nulink_usb_api.nulink_usb_init_buffer(handle, 4 * 1);
	/* set command ID */
	h_u32_to_le(h->cmdbuf + h->cmdidx, CMD_CHECK_ID);
	h->cmdidx += 4;

	int res = m_nulink_usb_api.nulink_usb_xfer(handle, h->databuf, 4 * 2);
	if (res != ERROR_OK)
		return res;

	*idcode = le_to_h_u32(h->databuf + 4 * 1);

	LOG_INFO("IDCODE: 0x%08" PRIX32, *idcode);

	return ERROR_OK;
}

static int nulink_usb_write_debug_reg(void *handle, uint32_t addr, uint32_t val)
{
	struct nulink_usb_handle_s *h = handle;

	LOG_DEBUG("nulink_usb_write_debug_reg 0x%08" PRIX32 " 0x%08" PRIX32, addr, val);

	m_nulink_usb_api.nulink_usb_init_buffer(handle, 8 + 12 * 1);
	/* set command ID */
	h_u32_to_le(h->cmdbuf + h->cmdidx, CMD_WRITE_RAM);
	h->cmdidx += 4;
	/* Count of registers */
	h->cmdbuf[h->cmdidx] = 1;
	h->cmdidx += 1;
	/* Array of bool value (u8ReadOld) */
	h->cmdbuf[h->cmdidx] = 0x00;
	h->cmdidx += 1;
	/* Array of bool value (u8Verify) */
	h->cmdbuf[h->cmdidx] = 0x00;
	h->cmdidx += 1;
	/* ignore */
	h->cmdbuf[h->cmdidx] = 0;
	h->cmdidx += 1;
	/* u32Addr */
	h_u32_to_le(h->cmdbuf + h->cmdidx, addr);
	h->cmdidx += 4;
	/* u32Data */
	h_u32_to_le(h->cmdbuf + h->cmdidx, val);
	h->cmdidx += 4;
	/* u32Mask */
	h_u32_to_le(h->cmdbuf + h->cmdidx, 0x00000000UL);
	h->cmdidx += 4;

	return m_nulink_usb_api.nulink_usb_xfer(handle, h->databuf, 4 * 2);
}

static enum target_state nulink_usb_state(void *handle)
{
	struct nulink_usb_handle_s *h = handle;

	assert(handle);

	m_nulink_usb_api.nulink_usb_init_buffer(handle, 4 * 1);
	/* set command ID */
	h_u32_to_le(h->cmdbuf + h->cmdidx, CMD_CHECK_MCU_STOP);
	h->cmdidx += 4;

	int res = m_nulink_usb_api.nulink_usb_xfer(handle, h->databuf, 4 * 3);
	if (res != ERROR_OK)
		return TARGET_UNKNOWN;

	if (!le_to_h_u32(h->databuf + 4 * 2))
		return TARGET_HALTED;
	else
		return TARGET_RUNNING;
	// untouch
	return TARGET_UNKNOWN;
}

static int nulink_usb_assert_srst(void *handle, int srst)
{
	struct nulink_usb_handle_s *h = handle;

	LOG_DEBUG("nulink_usb_assert_srst");

	assert(handle);

	m_nulink_usb_api.nulink_usb_init_buffer(handle, 4 * 4);
	/* set command ID */
	h_u32_to_le(h->cmdbuf + h->cmdidx, CMD_MCU_RESET);
	h->cmdidx += 4;
	/* set reset type */
	h_u32_to_le(h->cmdbuf + h->cmdidx, RESET_SYSRESETREQ);
	h->cmdidx += 4;
	/* set connect type */
	h_u32_to_le(h->cmdbuf + h->cmdidx, CONNECT_NORMAL);
	h->cmdidx += 4;
	/* set extMode */
	h_u32_to_le(h->cmdbuf + h->cmdidx, h->extMode_command);
	h->cmdidx += 4;

	return m_nulink_usb_api.nulink_usb_xfer(handle, h->databuf, 4 * 1);
}

static int nulink_usb_reset(void *handle)
{
	struct nulink_usb_handle_s *h = handle;

	switch (h->reset_command) {
		case RESET_AUTO:
			LOG_DEBUG("nulink_usb_reset: RESET_AUTO");
			break;
		case RESET_HW:
			LOG_DEBUG("nulink_usb_reset: RESET_HW");
			break;
		case RESET_SYSRESETREQ:
			LOG_DEBUG("nulink_usb_reset: RESET_SYSRESETREQ");
			break;
		case RESET_VECTRESET:
			LOG_DEBUG("nulink_usb_reset: RESET_VECTRESET");
			break;
		case RESET_FAST_RESCUE:
			LOG_DEBUG("nulink_usb_reset: RESET_FAST_RESCUE");
			break;
		case RESET_NONE_NULINK:
			LOG_DEBUG("nulink_usb_reset: RESET_NONE_NULINK");
			break;
		case RESET_NONE2:
			LOG_DEBUG("nulink_usb_reset: RESET_NONE2");
			break;
		default:
			LOG_DEBUG("nulink_usb_reset: reset_command not found");
			break;
	}

	switch (h->extMode_command) {
		case EXTMODE_NORMAL:
			LOG_DEBUG("nulink_usb_reset: EXTMODE_NORMAL");
			break;
		case EXTMODE_M0A21:
			LOG_DEBUG("nulink_usb_reset: EXTMODE_M0A21");
			break;
		case EXTMODE_M030G:
			LOG_DEBUG("nulink_usb_reset: EXTMODE_M030G");
			break;
		default:
			LOG_DEBUG("nulink_usb_reset: extMode_command not found");
			break;
	}

	assert(handle != NULL);

	m_nulink_usb_api.nulink_usb_init_buffer(handle, 4 * 4);
	/* set command ID */
	h_u32_to_le(h->cmdbuf + h->cmdidx, CMD_MCU_RESET);
	h->cmdidx += 4;
	/* set reset type */
	h_u32_to_le(h->cmdbuf + h->cmdidx, h->reset_command);
	h->cmdidx += 4;
	/* set connect type */
	h_u32_to_le(h->cmdbuf + h->cmdidx, CONNECT_NORMAL);
	h->cmdidx += 4;
	/* set extMode */
	h_u32_to_le(h->cmdbuf + h->cmdidx, h->extMode_command);
	h->cmdidx += 4;

	return m_nulink_usb_api.nulink_usb_xfer(handle, h->databuf, 4 * 1);
}

int nulink_usb_M2351_erase(void)
{
	int res = ERROR_FAIL;
	struct nulink_usb_handle_s *h = m_nulink_usb_handle;

	LOG_DEBUG("nulink_usb_M2351_erase");

	if (m_nulink_usb_handle) {
		// SET_CONFIG for M2351
		m_nulink_usb_api.nulink_usb_init_buffer(m_nulink_usb_handle, 4 * 6);
		/* set command ID */
		h_u32_to_le(h->cmdbuf + h->cmdidx, CMD_SET_CONFIG);
		h->cmdidx += 4;
		/* set max SWD clock */
		h_u32_to_le(h->cmdbuf + h->cmdidx, 1000);
		h->cmdidx += 4;
		/* chip type: NUC_CHIP_TYPE_M2351 */
		h_u32_to_le(h->cmdbuf + h->cmdidx, 0x321);
		h->cmdidx += 4;
		/* IO voltage */
		h_u32_to_le(h->cmdbuf + h->cmdidx, 5000);
		h->cmdidx += 4;
		/* If supply voltage to target or not */
		h_u32_to_le(h->cmdbuf + h->cmdidx, 0);
		h->cmdidx += 4;
		/* USB_FUNC_E: USB_FUNC_HID_BULK */
		h_u32_to_le(h->cmdbuf + h->cmdidx, 2);
		h->cmdidx += 4;

		m_nulink_usb_api.nulink_usb_xfer(m_nulink_usb_handle, h->databuf, 4 * 3);

		// Erase whole chip
		m_nulink_usb_api.nulink_usb_init_buffer(m_nulink_usb_handle, 4 * 6);
		/* set command ID */
		h_u32_to_le(h->cmdbuf + h->cmdidx, CMD_ERASE_FLASHCHIP);
		h->cmdidx += 4;
		/* set count */
		h_u32_to_le(h->cmdbuf + h->cmdidx, 0);
		h->cmdidx += 4;
		/* set config 0 */
		h_u32_to_le(h->cmdbuf + h->cmdidx, 0xFFFFFFFF);
		h->cmdidx += 4;
		/* set config 1 */
		h_u32_to_le(h->cmdbuf + h->cmdidx, 0xFFFFFFFF);
		h->cmdidx += 4;
		/* set config 2 */
		h_u32_to_le(h->cmdbuf + h->cmdidx, 0xFFFFFFFF);
		h->cmdidx += 4;
		/* set config 3 */
		h_u32_to_le(h->cmdbuf + h->cmdidx, 0xFFFFFFFF);
		h->cmdidx += 4;

		res = m_nulink_usb_api.nulink_usb_xfer(m_nulink_usb_handle, h->databuf, 4 * 1);
	}
	else {
		LOG_DEBUG("m_nulink_usb_handle not found");
	}

	return res;
}

int nulink_usb_assert_reset(void)
{
	int res;
	struct nulink_usb_handle_s *h = m_nulink_usb_handle;

	LOG_DEBUG("nulink_usb_assert_reset");

	m_nulink_usb_api.nulink_usb_init_buffer(m_nulink_usb_handle, 4 * 4);
	/* set command ID */
	h_u32_to_le(h->cmdbuf + h->cmdidx, CMD_MCU_RESET);
	h->cmdidx += 4;
	/* set reset type */
	h_u32_to_le(h->cmdbuf + h->cmdidx, RESET_SYSRESETREQ);
	h->cmdidx += 4;
	/* set connect type */
	h_u32_to_le(h->cmdbuf + h->cmdidx, CONNECT_NORMAL);
	h->cmdidx += 4;
	/* set extMode */
	h_u32_to_le(h->cmdbuf + h->cmdidx, h->extMode_command);
	h->cmdidx += 4;

	res = m_nulink_usb_api.nulink_usb_xfer(m_nulink_usb_handle, h->databuf, 4 * 1);

	return res;
}

static int nulink_usb_run(void *handle)
{
	struct nulink_usb_handle_s *h = handle;

	LOG_DEBUG("nulink_usb_run");

	assert(handle);

	m_nulink_usb_api.nulink_usb_init_buffer(handle, 4 * 1);
	/* set command ID */
	h_u32_to_le(h->cmdbuf + h->cmdidx, CMD_MCU_FREE_RUN);
	h->cmdidx += 4;

	return m_nulink_usb_api.nulink_usb_xfer(handle, h->databuf, 4 * 1);
}

static int nulink_usb_halt(void *handle)
{
	struct nulink_usb_handle_s *h = handle;

	LOG_DEBUG("nulink_usb_halt");

	assert(handle);

	m_nulink_usb_api.nulink_usb_init_buffer(handle, 4 * 1);
	/* set command ID */
	h_u32_to_le(h->cmdbuf + h->cmdidx, CMD_MCU_STOP_RUN);
	h->cmdidx += 4;

	int res =  m_nulink_usb_api.nulink_usb_xfer(handle, h->databuf, 4 * 2);

	LOG_DEBUG("Nu-Link stop_pc(0x%08" PRIx32 ")", le_to_h_u32(h->databuf + 4));

	return res;
}

static int nulink_usb_step(void *handle)
{
	struct nulink_usb_handle_s *h = handle;

	LOG_DEBUG("nulink_usb_step");

	assert(handle);

	m_nulink_usb_api.nulink_usb_init_buffer(handle, 4 * 1);
	/* set command ID */
	h_u32_to_le(h->cmdbuf + h->cmdidx, CMD_MCU_STEP_RUN);
	h->cmdidx += 4;

	int res = m_nulink_usb_api.nulink_usb_xfer(handle, h->databuf, 4 * 2);

	LOG_DEBUG("Nu-Link pc (0x%08" PRIx32 ")", le_to_h_u32(h->databuf + 4));

	return res;
}

static int nulink_usb_read_reg(void *handle, unsigned int regsel, uint32_t *val)
{
	struct nulink_usb_handle_s *h = handle;

	assert(handle);

	m_nulink_usb_api.nulink_usb_init_buffer(handle, 8 + 12 * 1);
	/* set command ID */
	h_u32_to_le(h->cmdbuf + h->cmdidx, CMD_WRITE_REG);
	h->cmdidx += 4;
	/* Count of registers */
	h->cmdbuf[h->cmdidx] = 1;
	h->cmdidx += 1;
	/* Array of bool value (u8ReadOld) */
	h->cmdbuf[h->cmdidx] = 0xFF;
	h->cmdidx += 1;
	/* Array of bool value (u8Verify) */
	h->cmdbuf[h->cmdidx] = 0x00;
	h->cmdidx += 1;
	/* ignore */
	h->cmdbuf[h->cmdidx] = 0;
	h->cmdidx += 1;
	/* u32Addr */
	h_u32_to_le(h->cmdbuf + h->cmdidx, regsel);
	h->cmdidx += 4;
	/* u32Data */
	h_u32_to_le(h->cmdbuf + h->cmdidx, 0);
	h->cmdidx += 4;
	/* u32Mask */
	h_u32_to_le(h->cmdbuf + h->cmdidx, 0xFFFFFFFFUL);
	h->cmdidx += 4;

	int res = m_nulink_usb_api.nulink_usb_xfer(handle, h->databuf, 4 * 2);

	*val = le_to_h_u32(h->databuf + 4 * 1);

	return res;
}

static int nulink_usb_write_reg(void *handle, unsigned int regsel, uint32_t val)
{
	struct nulink_usb_handle_s *h = handle;

	assert(handle);

	m_nulink_usb_api.nulink_usb_init_buffer(handle, 8 + 12 * 1);
	/* set command ID */
	h_u32_to_le(h->cmdbuf + h->cmdidx, CMD_WRITE_REG);
	h->cmdidx += 4;
	/* Count of registers */
	h->cmdbuf[h->cmdidx] = 1;
	h->cmdidx += 1;
	/* Array of bool value (u8ReadOld) */
	h->cmdbuf[h->cmdidx] = 0x00;
	h->cmdidx += 1;
	/* Array of bool value (u8Verify) */
	h->cmdbuf[h->cmdidx] = 0x00;
	h->cmdidx += 1;
	/* ignore */
	h->cmdbuf[h->cmdidx] = 0;
	h->cmdidx += 1;
	/* u32Addr */
	h_u32_to_le(h->cmdbuf + h->cmdidx, regsel);
	h->cmdidx += 4;
	/* u32Data */
	h_u32_to_le(h->cmdbuf + h->cmdidx, val);
	h->cmdidx += 4;
	/* u32Mask */
	h_u32_to_le(h->cmdbuf + h->cmdidx, 0x00000000UL);
	h->cmdidx += 4;

	return m_nulink_usb_api.nulink_usb_xfer(handle, h->databuf, 4 * 2);
}

static int nulink_usb_read_mem8(void *handle, uint32_t addr, uint16_t len,
		uint8_t *buffer)
{
	int res = ERROR_OK;
	uint32_t offset = 0;
	uint32_t bytes_remaining = 4;
	struct nulink_usb_handle_s *h = handle;

	LOG_DEBUG("nulink_usb_read_mem8: addr (0x%08" PRIx32 "), len %" PRId16, addr, len);

	assert(handle);

	/* check whether data is word aligned */
	if (addr % 4) {
		uint32_t aligned_addr = addr / 4;
		aligned_addr = aligned_addr * 4;
		offset = addr - aligned_addr;
		LOG_DEBUG("nulink_usb_read_mem8: unaligned address addr 0x%08" PRIx32
				"/aligned addr 0x%08" PRIx32 " offset %" PRIu32,
				addr, aligned_addr, offset);

		addr = aligned_addr;
	}

	while (len) {
		unsigned int count;

		if (len < bytes_remaining)
			bytes_remaining = len;

		if (len < 4)
			count = 1;
		else
			count = 2;

		m_nulink_usb_api.nulink_usb_init_buffer(handle, 8 + 12 * count);
		/* set command ID */
		h_u32_to_le(h->cmdbuf + h->cmdidx, CMD_WRITE_RAM);
		h->cmdidx += 4;
		/* Count of registers */
		h->cmdbuf[h->cmdidx] = count;
		h->cmdidx += 1;
		/* Array of bool value (u8ReadOld) */
		h->cmdbuf[h->cmdidx] = 0xFF;
		h->cmdidx += 1;
		/* Array of bool value (u8Verify) */
		h->cmdbuf[h->cmdidx] = 0x00;
		h->cmdidx += 1;
		/* ignore */
		h->cmdbuf[h->cmdidx] = 0;
		h->cmdidx += 1;

		for (unsigned int i = 0; i < count; i++) {
			/* u32Addr */
			h_u32_to_le(h->cmdbuf + h->cmdidx, addr);
			h->cmdidx += 4;
			/* u32Data */
			h_u32_to_le(h->cmdbuf + h->cmdidx, 0);
			h->cmdidx += 4;
			/* u32Mask */
			h_u32_to_le(h->cmdbuf + h->cmdidx, 0xFFFFFFFFUL);
			h->cmdidx += 4;
			/* proceed to the next one  */
			addr += 4;
		}

		res = m_nulink_usb_api.nulink_usb_xfer(handle, h->databuf, 4 * count * 2);
		if (res != ERROR_OK)
			break;

		/* fill in the output buffer */
		for (unsigned int i = 0; i < count; i++) {
			if (i == 0)
				memcpy(buffer, h->databuf + 4 + offset, len);
			else
				memcpy(buffer + 2 * i, h->databuf + 4 * (2 * i + 1), len - 2);
		}

		if (len >= bytes_remaining)
			len -= bytes_remaining;
		else
			len = 0;
	}

	return res;
}

static int nulink_usb_write_mem8(void *handle, uint32_t addr, uint16_t len,
		const uint8_t *buffer)
{
	int res = ERROR_OK;
	uint32_t offset = 0;
	uint32_t bytes_remaining = 12;
	struct nulink_usb_handle_s *h = handle;

	LOG_DEBUG("nulink_usb_write_mem8: addr(0x%08" PRIx32 "), len %" PRIu16, addr, len);

	assert(handle);

	/* check whether data is word aligned */
	if (addr % 4) {
		uint32_t aligned_addr = addr / 4;
		aligned_addr = aligned_addr * 4;
		offset = addr - aligned_addr;
		LOG_DEBUG("nulink_usb_write_mem8: address not aligned. addr(0x%08" PRIx32
				")/aligned_addr(0x%08" PRIx32 ")/offset(%" PRIu32 ")",
				addr, aligned_addr, offset);

		addr = aligned_addr;
	}

	while (len) {
		unsigned int count;

		if (len < bytes_remaining)
			bytes_remaining = len;

		if (len + offset <= 4)
			count = 1;
		else
			count = 2;

		m_nulink_usb_api.nulink_usb_init_buffer(handle, 8 + 12 * count);
		/* set command ID */
		h_u32_to_le(h->cmdbuf + h->cmdidx, CMD_WRITE_RAM);
		h->cmdidx += 4;
		/* Count of registers */
		h->cmdbuf[h->cmdidx] = count;
		h->cmdidx += 1;
		/* Array of bool value (u8ReadOld) */
		h->cmdbuf[h->cmdidx] = 0x00;
		h->cmdidx += 1;
		/* Array of bool value (u8Verify) */
		h->cmdbuf[h->cmdidx] = 0x00;
		h->cmdidx += 1;
		/* ignore */
		h->cmdbuf[h->cmdidx] = 0;
		h->cmdidx += 1;

		for (unsigned int i = 0; i < count; i++) {
			/* u32Addr */
			h_u32_to_le(h->cmdbuf + h->cmdidx, addr);
			h->cmdidx += 4;
			/* u32Data */
			uint32_t u32buffer = buf_get_u32(buffer, 0, len * 8);
			u32buffer = (u32buffer << offset * 8);
			h_u32_to_le(h->cmdbuf + h->cmdidx, u32buffer);
			h->cmdidx += 4;
			/* u32Mask */
			if (i == 0) {
				if (offset == 0) {
					if (len == 1) {
						h_u32_to_le(h->cmdbuf + h->cmdidx, 0xFFFFFF00UL);
						LOG_DEBUG("nulink_usb_write_mem8: count(%u), mask: 0xFFFFFF00", i);
					} else if (len == 2) {
						h_u32_to_le(h->cmdbuf + h->cmdidx, 0xFFFF0000UL);
						LOG_DEBUG("nulink_usb_write_mem8: count(%u), mask: 0xFFFF0000", i);
					}
					else { // len == 3
						h_u32_to_le(h->cmdbuf + h->cmdidx, 0xFF000000UL);
						LOG_DEBUG("nulink_usb_write_mem8: count(%u), mask: 0xFF000000", i);
					}
				} else if (offset == 1) {
					if (len == 1) {
						h_u32_to_le(h->cmdbuf + h->cmdidx, 0xFFFF00FFUL);
						LOG_DEBUG("nulink_usb_write_mem8: count(%u), mask: 0xFFFF00FF", i);
					} else if (len == 2) {
						h_u32_to_le(h->cmdbuf + h->cmdidx, 0xFF0000FFUL);
						LOG_DEBUG("nulink_usb_write_mem8: count(%u), mask: 0xFF0000FF", i);
					}
					else { // len == 3
						h_u32_to_le(h->cmdbuf + h->cmdidx, 0x000000FFUL);
						LOG_DEBUG("nulink_usb_write_mem8: count(%u), mask: 0x000000FF", i);
					}
				} else if (offset == 2) {
					if (len == 1) {
						h_u32_to_le(h->cmdbuf + h->cmdidx, 0xFF00FFFFUL);
						LOG_DEBUG("nulink_usb_write_mem8: count(%u), mask: 0xFF00FFFF", i);
					} else { // len == 2
						h_u32_to_le(h->cmdbuf + h->cmdidx, 0x0000FFFFUL);
						LOG_DEBUG("nulink_usb_write_mem8: count(%u), mask: 0x0000FFFF", i);
					}
				} else { // offset == 3
					if (len == 1) {
						h_u32_to_le(h->cmdbuf + h->cmdidx, 0x00FFFFFFUL);
						LOG_DEBUG("nulink_usb_write_mem8: count(%u), mask: 0x00FFFFFF", i);
					}
				}
			} else {
				if (offset == 1) {
					// len == 4
					h_u32_to_le(h->cmdbuf + h->cmdidx, 0xFFFFFF00UL);
					LOG_DEBUG("nulink_usb_write_mem8: count(%u), mask: 0xFFFFFF00", i);
				} else if (offset == 2) {
					if (len == 3) {
						h_u32_to_le(h->cmdbuf + h->cmdidx, 0xFFFFFF00UL);
						LOG_DEBUG("nulink_usb_write_mem8: count(%u), mask: 0xFFFFFF00", i);
					} else { // len == 4
						h_u32_to_le(h->cmdbuf + h->cmdidx, 0xFFFF0000UL);
						LOG_DEBUG("nulink_usb_write_mem8: count(%u), mask: 0xFFFF0000", i);
					}
				} else { // offset == 3
					if (len == 2) {
						h_u32_to_le(h->cmdbuf + h->cmdidx, 0xFFFFFF00UL);
						LOG_DEBUG("nulink_usb_write_mem8: count(%u), mask: 0xFFFFFF00", i);
					} else if (len == 3) {
						h_u32_to_le(h->cmdbuf + h->cmdidx, 0xFFFF0000UL);
						LOG_DEBUG("nulink_usb_write_mem8: count(%u), mask: 0xFFFF0000", i);
					} else { // len == 4
						h_u32_to_le(h->cmdbuf + h->cmdidx, 0xFF000000UL);
						LOG_DEBUG("nulink_usb_write_mem8: count(%u), mask: 0xFF000000", i);
					}
				}
			}
			h->cmdidx += 4;

			/* proceed to the next one */
			addr += 4;
			buffer += 4;
		}

		res = m_nulink_usb_api.nulink_usb_xfer(handle, h->databuf, 4 * count * 2);

		if (len >= bytes_remaining)
			len -= bytes_remaining;
		else
			len = 0;
	}

	return res;
}

static int nulink_usb_read_mem32(void *handle, uint32_t addr, uint16_t len,
		uint8_t *buffer)
{
	int res = ERROR_OK;
	uint32_t bytes_remaining = 12;
	struct nulink_usb_handle_s *h = handle;

	assert(handle);

	/* data must be a multiple of 4 and word aligned */
	if (len % 4 || addr % 4) {
		LOG_ERROR("Invalid data alignment");
		return ERROR_TARGET_UNALIGNED_ACCESS;
	}

	while (len) {
		if (len < bytes_remaining)
			bytes_remaining = len;

		unsigned int count = bytes_remaining / 4;

		m_nulink_usb_api.nulink_usb_init_buffer(handle, 8 + 12 * count);
		/* set command ID */
		h_u32_to_le(h->cmdbuf + h->cmdidx, CMD_WRITE_RAM);
		h->cmdidx += 4;
		/* Count of registers */
		h->cmdbuf[h->cmdidx] = count;
		h->cmdidx += 1;
		/* Array of bool value (u8ReadOld) */
		h->cmdbuf[h->cmdidx] = 0xFF;
		h->cmdidx += 1;
		/* Array of bool value (u8Verify) */
		h->cmdbuf[h->cmdidx] = 0x00;
		h->cmdidx += 1;
		/* ignore */
		h->cmdbuf[h->cmdidx] = 0;
		h->cmdidx += 1;

		for (unsigned int i = 0; i < count; i++) {
			/* u32Addr */
			h_u32_to_le(h->cmdbuf + h->cmdidx, addr);
			h->cmdidx += 4;
			/* u32Data */
			h_u32_to_le(h->cmdbuf + h->cmdidx, 0);
			h->cmdidx += 4;
			/* u32Mask */
			h_u32_to_le(h->cmdbuf + h->cmdidx, 0xFFFFFFFFUL);
			h->cmdidx += 4;
			/* proceed to the next one  */
			addr += 4;
		}

		res = m_nulink_usb_api.nulink_usb_xfer(handle, h->databuf, 4 * count * 2);

		/* fill in the output buffer */
		for (unsigned int i = 0; i < count; i++) {
			memcpy(buffer, h->databuf + 4 * (2 * i + 1), 4);
			buffer += 4;
		}

		if (len >= bytes_remaining)
			len -= bytes_remaining;
		else
			len = 0;
	}

	return res;
}

static int nulink_usb_write_mem32(void *handle, uint32_t addr, uint16_t len,
		const uint8_t *buffer)
{
	int res = ERROR_OK;
	uint32_t bytes_remaining = 12;
	struct nulink_usb_handle_s *h = handle;

	assert(handle);

	/* data must be a multiple of 4 and word aligned */
	if (len % 4 || addr % 4) {
		LOG_ERROR("Invalid data alignment");
		return ERROR_TARGET_UNALIGNED_ACCESS;
	}

	while (len) {
		if (len < bytes_remaining)
			bytes_remaining = len;

		unsigned int count = bytes_remaining / 4;

		m_nulink_usb_api.nulink_usb_init_buffer(handle, 8 + 12 * count);
		/* set command ID */
		h_u32_to_le(h->cmdbuf + h->cmdidx, CMD_WRITE_RAM);
		h->cmdidx += 4;
		/* Count of registers */
		h->cmdbuf[h->cmdidx] = count;
		h->cmdidx += 1;
		/* Array of bool value (u8ReadOld) */
		h->cmdbuf[h->cmdidx] = 0x00;
		h->cmdidx += 1;
		/* Array of bool value (u8Verify) */
		h->cmdbuf[h->cmdidx] = 0x00;
		h->cmdidx += 1;
		/* ignore */
		h->cmdbuf[h->cmdidx] = 0;
		h->cmdidx += 1;

		for (unsigned int i = 0; i < count; i++) {
			/* u32Addr */
			h_u32_to_le(h->cmdbuf + h->cmdidx, addr);
			h->cmdidx += 4;
			/* u32Data */
			uint32_t u32buffer = buf_get_u32(buffer, 0, 32);
			h_u32_to_le(h->cmdbuf + h->cmdidx, u32buffer);
			h->cmdidx += 4;
			/* u32Mask */
			h_u32_to_le(h->cmdbuf + h->cmdidx, 0x00000000UL);
			h->cmdidx += 4;

			/* proceed to the next one */
			addr += 4;
			buffer += 4;
		}

		res =  m_nulink_usb_api.nulink_usb_xfer(handle, h->databuf, 4 * count * 2);

		if (len >= bytes_remaining)
			len -= bytes_remaining;
		else
			len = 0;
	}

	return res;
}

static uint32_t nulink_max_block_size(uint32_t tar_autoincr_block, uint32_t address)
{
	uint32_t max_tar_block = (tar_autoincr_block - ((tar_autoincr_block - 1) & address));

	if (max_tar_block == 0)
		max_tar_block = 4;

	return max_tar_block;
}

static int nulink_usb_read_mem(void *handle, uint32_t addr, uint32_t size,
		uint32_t count, uint8_t *buffer)
{
	int retval = ERROR_OK;
	struct nulink_usb_handle_s *h = handle;

	/* calculate byte count */
	count *= size;

	while (count) {
		uint32_t bytes_remaining = nulink_max_block_size(h->max_mem_packet, addr);

		if (count < bytes_remaining)
			bytes_remaining = count;

		if (bytes_remaining >= 4)
			size = 4;

		/* the nulink only supports 8/32bit memory read/writes
		 * honour 32bit, all others will be handled as 8bit access */
		if (size == 4) {
			/* When in jtag mode the nulink uses the auto-increment functionality.
			 * However it expects us to pass the data correctly, this includes
			 * alignment and any page boundaries. We already do this as part of the
			 * adi_v5 implementation, but the nulink is a hla adapter and so this
			 * needs implementing manually.
			 * currently this only affects jtag mode, they do single
			 * access in SWD mode - but this may change and so we do it for both modes */

			/* we first need to check for any unaligned bytes */
			if (addr % 4) {
				uint32_t head_bytes = 4 - (addr % 4);
				retval = nulink_usb_read_mem8(handle, addr, head_bytes, buffer);
				if (retval != ERROR_OK)
					return retval;
				buffer += head_bytes;
				addr += head_bytes;
				count -= head_bytes;
				bytes_remaining -= head_bytes;
			}

			if (bytes_remaining % 4)
				retval = nulink_usb_read_mem(handle, addr, 1, bytes_remaining, buffer);
			else
				retval = nulink_usb_read_mem32(handle, addr, bytes_remaining, buffer);
		} else {
			retval = nulink_usb_read_mem8(handle, addr, bytes_remaining, buffer);
		}

		if (retval != ERROR_OK)
			return retval;

		buffer += bytes_remaining;
		addr += bytes_remaining;
		count -= bytes_remaining;
	}

	return retval;
}

static int nulink_usb_write_mem(void *handle, uint32_t addr, uint32_t size,
		uint32_t count, const uint8_t *buffer)
{
	int retval = ERROR_OK;
	extern char *m_target_name;
	struct nulink_usb_handle_s *h = handle;

	if (addr < ARM_SRAM_BASE) {
		if (strcmp(m_target_name, "NUC505") != 0) {
			LOG_DEBUG("nulink_usb_write_mem: address below ARM_SRAM_BASE, not supported.\n");
			return retval;
		} else {
			LOG_DEBUG("although the address is below ARM_SRAM_BASE, the Nuvoton %s chip supports this kind of writing.", m_target_name);
		}
	}

	/* calculate byte count */
	count *= size;

	while (count) {
		uint32_t bytes_remaining = nulink_max_block_size(h->max_mem_packet, addr);

		if (count < bytes_remaining)
			bytes_remaining = count;

		if (bytes_remaining >= 4)
			size = 4;

		/* the nulink only supports 8/32bit memory read/writes
		 * honour 32bit, all others will be handled as 8bit access */
		if (size == 4) {
			/* When in jtag mode the nulink uses the auto-increment functionality.
			 * However it expects us to pass the data correctly, this includes
			 * alignment and any page boundaries. We already do this as part of the
			 * adi_v5 implementation, but the nulink is a hla adapter and so this
			 * needs implementing manually.
			 * currently this only affects jtag mode, do single
			 * access in SWD mode - but this may change and so we do it for both modes */

			/* we first need to check for any unaligned bytes */
			if (addr % 4) {
				uint32_t head_bytes = 4 - (addr % 4);
				retval = nulink_usb_write_mem8(handle, addr, head_bytes, buffer);
				if (retval != ERROR_OK)
					return retval;
				buffer += head_bytes;
				addr += head_bytes;
				count -= head_bytes;
				bytes_remaining -= head_bytes;
			}

			if (bytes_remaining % 4)
				retval = nulink_usb_write_mem(handle, addr, 1, bytes_remaining, buffer);
			else
				retval = nulink_usb_write_mem32(handle, addr, bytes_remaining, buffer);

		} else {
			retval = nulink_usb_write_mem8(handle, addr, bytes_remaining, buffer);
		}

		if (retval != ERROR_OK)
			return retval;

		buffer += bytes_remaining;
		addr += bytes_remaining;
		count -= bytes_remaining;
	}

	return retval;
}

static int nulink_usb_override_target(const char *targetname)
{
	LOG_DEBUG("nulink_usb_override_target");

	return !strcmp(targetname, "cortex_m");
}

static char *nulink_usb_get_alternate_serial(struct libusb_device_handle *device,
		struct libusb_device_descriptor *dev_desc)
{
	int usb_retval;
	unsigned char desc_serial[(NULINK_SERIAL_LEN + 1) * 2];

	if (dev_desc->iSerialNumber == 0)
		return NULL;

	/* get the LANGID from String Descriptor Zero */
	usb_retval = libusb_get_string_descriptor(device, 0, 0, desc_serial,
			sizeof(desc_serial));

	if (usb_retval < LIBUSB_SUCCESS) {
		LOG_ERROR("libusb_get_string_descriptor() failed: %s(%d)",
				libusb_error_name(usb_retval), usb_retval);
		return NULL;
	} else if (usb_retval < 4) {
		/* the size should be least 4 bytes to contain a minimum of 1 supported LANGID */
		LOG_ERROR("could not get the LANGID");
		return NULL;
	}

	uint32_t langid = desc_serial[2] | (desc_serial[3] << 8);

	/* get the serial */
	usb_retval = libusb_get_string_descriptor(device, dev_desc->iSerialNumber,
			langid, desc_serial, sizeof(desc_serial));

	unsigned char len = desc_serial[0];

	if (usb_retval < LIBUSB_SUCCESS) {
		LOG_ERROR("libusb_get_string_descriptor() failed: %s(%d)",
				libusb_error_name(usb_retval), usb_retval);
		return NULL;
	} else if (desc_serial[1] != LIBUSB_DT_STRING || len > usb_retval) {
		LOG_ERROR("invalid string in ST-LINK USB serial descriptor");
		return NULL;
	}

	if (len == ((NULINK_SERIAL_LEN + 1) * 2)) {
		/* good NU-Link adapter, this case is managed by
		 * libusb::libusb_get_string_descriptor_ascii */
		return NULL;
	} else if (len != ((NULINK_SERIAL_LEN / 2 + 1) * 2)) {
		LOG_ERROR("unexpected serial length (%d) in descriptor", len);
		return NULL;
	}

	/* else (len == 26) => buggy NU-Link */

	char *alternate_serial = malloc((NULINK_SERIAL_LEN + 1) * sizeof(char));
	if (!alternate_serial)
		return NULL;

	for (unsigned int i = 0; i < NULINK_SERIAL_LEN; i += 2)
		sprintf(alternate_serial + i, "%02X", desc_serial[i + 2]);

	alternate_serial[NULINK_SERIAL_LEN] = '\0';

	return alternate_serial;
}

static int nulink_speed(void *handle, int khz, bool query)
{
	struct nulink_usb_handle_s *h = handle;
	unsigned long max_ice_clock = khz;

	LOG_DEBUG("nulink_speed: query %s", query ? "yes" : "no");

	if (max_ice_clock > 12000)
		max_ice_clock = 12000;
	else if ((max_ice_clock == 3 * 512) || (max_ice_clock == 1500))
		max_ice_clock = 1500;
	else if (max_ice_clock >= 1000)
		max_ice_clock = max_ice_clock / 1000 * 1000;
	else
		max_ice_clock = max_ice_clock / 100 * 100;

	LOG_DEBUG("Nu-Link nulink_speed: %lu", max_ice_clock);

	if (!query) {
		m_nulink_usb_api.nulink_usb_init_buffer(handle, 4 * 6);
		/* set command ID */
		h_u32_to_le(h->cmdbuf + h->cmdidx, CMD_SET_CONFIG);
		h->cmdidx += 4;
		/* set max SWD clock */
		h_u32_to_le(h->cmdbuf + h->cmdidx, max_ice_clock);
		h->cmdidx += 4;
		/* chip type: */
		h_u32_to_le(h->cmdbuf + h->cmdidx, h->chip_type);
		h->cmdidx += 4;
		/* IO voltage */
		h_u32_to_le(h->cmdbuf + h->cmdidx, h->io_voltage);
		h->cmdidx += 4;
		/* If supply voltage to target or not */
		h_u32_to_le(h->cmdbuf + h->cmdidx, 0);
		h->cmdidx += 4;
		/* USB_FUNC_E: USB_FUNC_HID_BULK */
		h_u32_to_le(h->cmdbuf + h->cmdidx, 2);
		h->cmdidx += 4;

		m_nulink_usb_api.nulink_usb_xfer(handle, h->databuf, 4 * 3);

		LOG_DEBUG("nulink_speed: h->hardwareConfig(%" PRId16 ")", h->hardwareConfig);
		if (h->hardwareConfig & HARDWARE_CONFIG_NULINKPRO) {
			LOG_INFO("Nu-Link target_voltage_mv[0](%04" PRIx16 "), target_voltage_mv[1](%04" PRIx16
				"), target_voltage_mv[2](%04" PRIx16 "), if_target_power_supplied(%d)",
				le_to_h_u16(h->databuf + 4 * 1 + 0),
				le_to_h_u16(h->databuf + 4 * 1 + 2),
				le_to_h_u16(h->databuf + 4 * 2 + 0),
				le_to_h_u16(h->databuf + 4 * 2 + 2) & 1);
		}
		/* wait for NUC505 IBR operations */
		busy_sleep(50);
	}


	return max_ice_clock;
}

static int nulink_usb_close(void *handle)
{
	struct nulink_usb_handle_s *h = handle;

	LOG_DEBUG("nulink_usb_close");

	if (handle != NULL) {
		LOG_DEBUG("trying to disconnect with nulink");
		m_nulink_usb_api.nulink_usb_init_buffer(handle, 4 * 4);
		/* set command ID */
		h_u32_to_le(h->cmdbuf + h->cmdidx, CMD_MCU_RESET);
		h->cmdidx += 4;
		/* set reset type */
		h_u32_to_le(h->cmdbuf + h->cmdidx, RESET_NONE_NULINK);
		h->cmdidx += 4;
		/* set connect type */
		h_u32_to_le(h->cmdbuf + h->cmdidx, CONNECT_DISCONNECT);
		h->cmdidx += 4;
		/* set extMode */
		h_u32_to_le(h->cmdbuf + h->cmdidx, h->extMode_command);
		h->cmdidx += 4;

		m_nulink_usb_api.nulink_usb_xfer(handle, h->databuf, 4 * 1);
	}


	return ERROR_OK;
}

static int nulink_usb_open(struct hl_interface_param_s *param, void **fd)
{
	int err, result = 0;
	struct nulink_usb_handle_s *h;

	LOG_DEBUG("nulink_usb_open");
	char buf[512];

	m_nulink_usb_handle = NULL;
#ifdef _WIN32
	struct stat fileStat;
	err = stat("c:\\Program Files\\Nuvoton Tools\\OpenOCD\\bin\\NuLink.exe", &fileStat);
	LOG_DEBUG("Stat Case 1: %d", err);
	if(err >= 0) {
		sprintf(buf, "\"c:\\Program Files\\Nuvoton Tools\\OpenOCD\\bin\\NuLink.exe\" -o conflict");
		result = system(buf);
		LOG_DEBUG("Run NuLink.exe on Win32 (result: %d)", result);
		if (result == -46) {
			LOG_DEBUG("A conflict happened! (result: %d)", result);
			LOG_ERROR("The ICE has been used by another Nuvoton tool. OpenOCD cannot work with the ICE unless we close the connection between the ICE and Nuvoton tool.");
			return ERROR_FAIL;
		}
		else if (result == -6) {
			LOG_DEBUG("Cannot find a target chip! (result: %d)", result);
			LOG_ERROR("We cannot find any Nuvoton device. Please check the hardware connection.");
			LOG_ERROR("If the ICE is used by another Nuvoton tool, please close the connection between the ICE and Nuvoton tool.");
			return ERROR_FAIL;
		}
		else if (result == 0) {
			sprintf(buf, "start /b \"\" \"c:\\Program Files\\Nuvoton Tools\\OpenOCD\\bin\\NuLink.exe\" -o wait");
			result = system(buf);
			busy_sleep(500);
			LOG_DEBUG("Wait NuLink.exe (result: %d)", result);
		}
	}
	else {
		err = stat("c:\\Program Files (x86)\\Nuvoton Tools\\OpenOCD\\bin\\NuLink.exe", &fileStat);
		LOG_DEBUG("Stat Case 2: %d", err);
		if(err >= 0) {
			sprintf(buf, "\"c:\\Program Files (x86)\\Nuvoton Tools\\OpenOCD\\bin\\NuLink.exe\" -o conflict");
			result = system(buf);
			LOG_DEBUG("Run NuLink.exe on Win64 (result: %d)", result);
			if (result == -46) {
				LOG_DEBUG("A conflict happened! (result: %d)", result);
				LOG_ERROR("The ICE has been used by another Nuvoton Tool. OpenOCD cannot work with the ICE unless we close the connection between the ICE and Nuvoton tool.");
				return ERROR_FAIL;
			}
			else if (result == -6) {
				LOG_DEBUG("Cannot find a target chip! (result: %d)", result);
				LOG_ERROR("We cannot find any Nuvoton device. Please check the hardware connection.");
				LOG_ERROR("If the ICE is used by another Nuvoton tool, please close the connection between the ICE and Nuvoton tool.");
				return ERROR_FAIL;
			}
			else if (result == 0) {
				sprintf(buf, "start /b \"\" \"c:\\Program Files (x86)\\Nuvoton Tools\\OpenOCD\\bin\\NuLink.exe\" -o wait");
				result = system(buf);
				busy_sleep(500);
				LOG_DEBUG("Wait NuLink.exe (result: %d)", result);
			}
		}
		else {
			LOG_DEBUG("Skip running NuLink.exe");
		}
	}
#endif

	h = calloc(1, sizeof(struct nulink_usb_handle_s));

	if (h == 0) {
		LOG_ERROR("malloc failed");
		return ERROR_FAIL;
	}

	h->transport = param->transport;

	const uint16_t vid_nulink2[] = { 0x0416, 0x0416, 0x0416, 0x0416, 0x0416, 0x0416, 0x0416, 0x0416, 0x0416, 0x0416, 0x0416, 0 };
	const uint16_t pid_nulink2[] = { 0x5200, 0x5201, 0x5202, 0x5203, 0x5204, 0x5205, 0x2004, 0x2005, 0x2006, 0x2007, 0x2008, 0 };

	/* get the Nu-Link version */
	if (jtag_libusb_open(vid_nulink2, pid_nulink2, &h->fd, nulink_usb_get_alternate_serial) == ERROR_OK) {
		h->hardwareConfig |= HARDWARE_CONFIG_NULINK2;
		m_nulink_usb_api.nulink_usb_xfer = nulink2_usb_xfer;
		m_nulink_usb_api.nulink_usb_init_buffer = nulink2_usb_init_buffer;
		h->interface_num = NULINK2_INTERFACE_NUM;
		h->max_packet_size = jtag_libusb_get_maxPacketSize(h->fd, 0, h->interface_num, &h->rx_ep, &h->tx_ep);
		if (h->max_packet_size == (uint16_t)-1) {
			h->max_packet_size = NULINK2_HID_MAX_SIZE;
		}
		LOG_INFO("NULINK is Nu-Link2");
	}
	else {
		if (jtag_libusb_open(param->vid, param->pid, &h->fd, nulink_usb_get_alternate_serial) != ERROR_OK) {
			LOG_ERROR("open failed");
			goto error_open;
		}

		m_nulink_usb_api.nulink_usb_xfer = nulink1_usb_xfer;
		m_nulink_usb_api.nulink_usb_init_buffer = nulink1_usb_init_buffer;
		h->interface_num = NULINK_INTERFACE_NUM;

		h->max_packet_size = jtag_libusb_get_maxPacketSize(h->fd, 0, h->interface_num, &h->rx_ep, &h->tx_ep);
		if (h->max_packet_size == (uint16_t)-1) {
			h->max_packet_size = NULINK_HID_MAX_SIZE;
		}
		LOG_INFO("NULINK is Nu-Link1");
	}
	LOG_DEBUG("interface number: %d", h->interface_num);
	LOG_DEBUG("rx endpoint: 0x%02x", h->rx_ep);
	LOG_DEBUG("tx endpoint: 0x%02x", h->tx_ep);
	LOG_DEBUG("max_packet_size: %d", h->max_packet_size);
	LOG_DEBUG("jtag_libusb_open succeeded");

	jtag_libusb_set_configuration(h->fd, 0);

	err = libusb_detach_kernel_driver(h->fd, h->interface_num);
	if (err != ERROR_OK) {
		LOG_DEBUG("detach kernel driver failed(%d)", err);
		//goto error_open;
	}
	else {
		LOG_DEBUG("libusb_detach_kernel_driver succeeded");
	}

	err = libusb_claim_interface(h->fd, h->interface_num);
	if (err != ERROR_OK) {
		LOG_ERROR("claim interface failed(%d)", err);
		goto error_open;
	}
	else {
		LOG_DEBUG("libusb_claim_interface succeeded");
	}

	h->usbcmdidx = 0;
	h->hardwareConfig = 0;

	/* get the device version */
	h->cmdsize = 4 * 5;
	err = nulink_usb_version(h);
	if (err != ERROR_OK) {
		LOG_DEBUG("nulink_usb_version failed with cmdSize(4 * 5)");
		h->cmdsize = 4 * 6;
		err = nulink_usb_version(h);
		if (err != ERROR_OK) {
			LOG_DEBUG("nulink_usb_version failed with cmdSize(4 * 6)");
		}
	}

	if ((strcmp(param->device_desc, "Nu-Link-Pro output voltage 1800") == 0) ||
		(strcmp(param->device_desc, "Nu-Link2-Pro output voltage 1800") == 0)) {
		h->io_voltage = 1800;
	}
	else if ((strcmp(param->device_desc, "Nu-Link-Pro output voltage 2500") == 0) ||
			 (strcmp(param->device_desc, "Nu-Link2-Pro output voltage 2500") == 0)) {
		h->io_voltage = 2500;
	}
	else if ((strcmp(param->device_desc, "Nu-Link-Pro output voltage 5000") == 0) ||
			 (strcmp(param->device_desc, "Nu-Link2-Pro output voltage 5000") == 0)) {
		h->io_voltage = 5000;
	}
	else {
		h->io_voltage = 3300;
	}

	/* SWD clock rate : 1MHz */
	/* chip type: NUC_CHIP_TYPE_GENERAL_V6M */
	h->chip_type = 0;
	nulink_speed(h, param->initial_interface_speed, false);

	LOG_DEBUG("nulink_usb_open: we manually perform nulink_usb_reset");
	h->reset_command = RESET_HW;
	h->extMode_command = EXTMODE_NORMAL;
	if (nulink_usb_reset(h) != ERROR_OK) {
		h->chip_type = NUC_CHIP_TYPE_M460;
		nulink_speed(h, param->initial_interface_speed, false);
		if (nulink_usb_reset(h) != ERROR_OK) {
			h->chip_type = 0;
			nulink_speed(h, param->initial_interface_speed, false);
			h->extMode_command = EXTMODE_M0A21;
			if (nulink_usb_reset(h) != ERROR_OK) {
				h->extMode_command = EXTMODE_M030G;
				if (nulink_usb_reset(h) != ERROR_OK) {
					LOG_ERROR("nulink_usb_reset failed");
					goto error_open;
				}
			}
		}
	}
	h->reset_command = RESET_SYSRESETREQ;
	nulink_usb_reset(h);

	/* get cpuid, so we can determine the max page size
	* start with a safe default for Cortex-M0*/
	h->max_mem_packet = (1 << 10);

	uint8_t buffer[4];
	err = nulink_usb_read_mem32(h, CPUID, 4, buffer);
	if (err == ERROR_OK) {
		uint32_t cpuid = le_to_h_u32(buffer);
		int i;

/* CPUID part number */
#define V6M_CPUID_PARTNO     0xC20
#define V7M_CPUID_PARTNO     0xC24
#define V8MBL_CPUID_PARTNO   0xD20
#define V8MML_CPUID_PARTNO   0xD21

		if (((cpuid >> 4) & 0xfff) == V8MBL_CPUID_PARTNO || ((cpuid >> 4) & 0xfff) == V8MML_CPUID_PARTNO) {
			i = 23;
		}
		else {
			i = (cpuid >> 4) & 0xf;
		}

#undef V6M_CPUID_PARTNO
#undef V7M_CPUID_PARTNO
#undef V8MBL_CPUID_PARTNO
#undef V8MML_CPUID_PARTNO

		if (i == 4 || i == 3 || i == 23) {
			/* Cortex-M3/M4/M23 has 4096 bytes autoincrement range */
			h->max_mem_packet = (1 << 12);
		}
	}

	LOG_DEBUG("max page size: %" PRIu32, h->max_mem_packet);

	*fd = h;
	m_nulink_usb_handle = h;

	return ERROR_OK;

error_open:

	if (h && h->fd)
		jtag_libusb_close(h->fd);

	free(h);

	return ERROR_FAIL;
}

static int nulink_usb_trace_read(void *handle, uint8_t *buf, size_t *size)
{
	/* not supported*/
	LOG_DEBUG("nulink_usb_trace_read is not supported");

	return ERROR_OK;
}

static int nulink_usb_read_regs(void *handle)
{
	/* not supported*/
	LOG_DEBUG("nulink_usb_read_regs");

	return ERROR_OK;
}

static int nulink_config_trace(void *handle, bool enabled,
				enum tpiu_pin_protocol pin_protocol, uint32_t port_size,
				unsigned int *trace_freq, unsigned int traceclkin_freq,
				uint16_t *prescaler)
{
	/* not supported */
	LOG_DEBUG("nulink_config_trace");

	return ERROR_OK;
}

struct hl_layout_api_s nulink_usb_layout_api = {
	.open = nulink_usb_open,
	.close = nulink_usb_close,
	.idcode = nulink_usb_idcode,
	.state = nulink_usb_state,
	.reset = nulink_usb_reset,
	.assert_srst = nulink_usb_assert_srst,
	.run = nulink_usb_run,
	.halt = nulink_usb_halt,
	.step = nulink_usb_step,
	.read_regs = nulink_usb_read_regs,
	.read_reg = nulink_usb_read_reg,
	.write_reg = nulink_usb_write_reg,
	.read_mem = nulink_usb_read_mem,
	.write_mem = nulink_usb_write_mem,
	.write_debug_reg = nulink_usb_write_debug_reg,
	.override_target = nulink_usb_override_target,
	.speed = nulink_speed,
	.config_trace = &nulink_config_trace,
	.poll_trace = nulink_usb_trace_read,
};
