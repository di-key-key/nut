/* cps-ncl.c - CyberPower proprietary NCL bank support
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "main.h"
#include "nut_libusb.h"
#include "usbhid-ups.h"
#include "cps-ncl.h"

#define CPS_NCL_VENDOR_ID               0x0764
#define CPS_NCL_PRODUCT_ID              0x0601
#define CPS_NCL_PRODUCT_NAME            "CP1200EIPFCRM2U"
#define CPS_NCL_FEATURE_ENABLE          0x25
#define CPS_NCL_FEATURE_READY           0x2c
#define CPS_NCL_EP_OUT                  0x02
#define CPS_NCL_COMMAND_REPORT          0x29
#define CPS_NCL_REPLY_REPORT            0x28
#define CPS_NCL_PACKET_SIZE             64
#define CPS_NCL_READY_RETRIES           50
#define CPS_NCL_REPLY_RETRIES           8
#define CPS_NCL_VERIFY_RETRIES          12
#define CPS_NCL_VERIFY_INTERVAL_US      250000

static int cps_ncl_enabled = 0;
static int cps_ncl_registered = 0;
static int cps_ncl_backend_warned = 0;

static int cps_ncl_supported_device(const HIDDevice_t *device)
{
	if (!device || !device->Product) {
		return 0;
	}

	return device->VendorID == CPS_NCL_VENDOR_ID
		&& device->ProductID == CPS_NCL_PRODUCT_ID
		&& !strcmp(device->Product, CPS_NCL_PRODUCT_NAME);
}

static int cps_ncl_transport_ready(void)
{
	usb_ctrl_char report[2];
	int ret;
	int attempt;

	if (!cps_ncl_enabled || comm_driver != &usb_subdriver
	 || udev == HID_DEV_HANDLE_CLOSED) {
		return 0;
	}

	memset(report, 0, sizeof(report));
	ret = comm_driver->get_report(udev, CPS_NCL_FEATURE_READY,
		report, sizeof(report));

	if (ret == 2
	 && (unsigned char)report[0] == CPS_NCL_FEATURE_READY
	 && (unsigned char)report[1] == 0x01) {
		return 1;
	}

	memset(report, 0, sizeof(report));
	report[0] = CPS_NCL_FEATURE_ENABLE;
	report[1] = 0x01;
	ret = comm_driver->set_report(udev, CPS_NCL_FEATURE_ENABLE,
		report, sizeof(report));

	if (ret != 2) {
		upslogx(LOG_WARNING,
			"%s: SET feature report 0x%02x failed, ret=%d",
			__func__, CPS_NCL_FEATURE_ENABLE, ret);
		return 0;
	}

	for (attempt = 0; attempt < CPS_NCL_READY_RETRIES; attempt++) {
		usleep(100000);
		memset(report, 0, sizeof(report));

		ret = comm_driver->get_report(udev, CPS_NCL_FEATURE_READY,
			report, sizeof(report));
		if (ret == 2
		 && (unsigned char)report[0] == CPS_NCL_FEATURE_READY
		 && (unsigned char)report[1] == 0x01) {
			return 1;
		}
	}

	upslogx(LOG_WARNING,
		"%s: CyberPower proprietary command channel did not become ready",
		__func__);
	return 0;
}

static int cps_ncl_command(const char *command, usb_ctrl_char *response,
	size_t response_size, size_t *response_len)
{
	usb_ctrl_char tx[CPS_NCL_PACKET_SIZE];
	usb_ctrl_char rx[CPS_NCL_PACKET_SIZE];
	size_t command_len;
	size_t payload_len;
	int ret;
	int attempt;

	if (response_len) {
		*response_len = 0;
	}

	if (!command) {
		return 0;
	}

	command_len = strlen(command);
	if (command_len == 0 || command_len > CPS_NCL_PACKET_SIZE - 2) {
		return 0;
	}

	if (!cps_ncl_transport_ready()) {
		return 0;
	}

	memset(tx, 0, sizeof(tx));
	tx[0] = CPS_NCL_COMMAND_REPORT;
	tx[1] = (usb_ctrl_char)command_len;
	memcpy(&tx[2], command, command_len);

	ret = usb_interrupt_write(udev, CPS_NCL_EP_OUT, tx,
		(int)sizeof(tx), 2000);
	if (ret != CPS_NCL_PACKET_SIZE) {
		upslogx(LOG_WARNING,
			"%s: CyberPower interrupt OUT failed, ret=%d",
			__func__, ret);
		return 0;
	}

	for (attempt = 0; attempt < CPS_NCL_REPLY_RETRIES; attempt++) {
		memset(rx, 0, sizeof(rx));
		ret = comm_driver->get_interrupt(udev, rx, sizeof(rx), 1000);
		if (ret <= 0) {
			continue;
		}

		/* Ordinary HID notifications may arrive before the proprietary
		 * response. Only report 0x28 belongs to this command channel. */
		if ((unsigned char)rx[0] != CPS_NCL_REPLY_REPORT) {
			continue;
		}

		if (ret < 2) {
			return 0;
		}

		payload_len = (unsigned char)rx[1];
		if (payload_len > CPS_NCL_PACKET_SIZE - 2
		 || payload_len > (size_t)(ret - 2)) {
			return 0;
		}

		if (response) {
			if (payload_len > response_size) {
				return 0;
			}
			memcpy(response, &rx[2], payload_len);
		}

		if (response_len) {
			*response_len = payload_len;
		}

		return 1;
	}

	upslogx(LOG_WARNING, "%s: no CyberPower command response", __func__);
	return 0;
}

static int cps_ncl_read_state(int *state_out)
{
	usb_ctrl_char response[CPS_NCL_PACKET_SIZE];
	unsigned char status_byte;
	size_t response_len = 0;
	int state;

	memset(response, 0, sizeof(response));
	if (!cps_ncl_command("D\r", response, sizeof(response), &response_len)) {
		return 0;
	}

	if (response_len < 3
	 || response[0] != '#'
	 || response[response_len - 1] != '\r') {
		upslogx(LOG_WARNING,
			"%s: malformed CyberPower status response", __func__);
		return 0;
	}

	/* On the CP1200EIPFCRM2U the least-significant bit of the byte
	 * immediately preceding CR reflects NCL Bank 1: 0=off, 1=on. */
	status_byte = (unsigned char)response[response_len - 2];
	state = (status_byte & 0x01) ? 1 : 0;
	dstate_setinfo("outlet.1.status", "%s", state ? "on" : "off");

	if (state_out) {
		*state_out = state;
	}

	return 1;
}

void cps_ncl_initinfo(HIDDevice_t *device)
{
	cps_ncl_enabled = cps_ncl_supported_device(device);
	if (!cps_ncl_enabled) {
		return;
	}

	/* The proprietary command path needs a raw USB interrupt OUT transfer.
	 * The native WinHID backend does not currently expose an equivalent. */
	if (comm_driver != &usb_subdriver) {
		if (!cps_ncl_backend_warned) {
			upslogx(LOG_WARNING,
				"CyberPower %s NCL control requires the libusb backend",
				CPS_NCL_PRODUCT_NAME);
			cps_ncl_backend_warned = 1;
		}
		cps_ncl_enabled = 0;
		return;
	}

	if (!cps_ncl_registered) {
		upslogx(LOG_INFO,
			"CyberPower %s: enabling NCL Bank 1 support",
			CPS_NCL_PRODUCT_NAME);

		dstate_setinfo("outlet.count", "1");
		dstate_setinfo("outlet.1.name", "NCL Bank 1");
		dstate_setinfo("outlet.1.desc", "NCL Bank 1");
		dstate_setinfo("outlet.1.switchable", "yes");
		dstate_addcmd("outlet.1.load.on");
		dstate_addcmd("outlet.1.load.off");
		cps_ncl_registered = 1;
	}

	if (!cps_ncl_read_state(NULL)) {
		upslogx(LOG_WARNING,
			"CyberPower %s: unable to read NCL Bank 1 state",
			CPS_NCL_PRODUCT_NAME);
	}
}

void cps_ncl_updateinfo(void)
{
	if (!cps_ncl_enabled) {
		return;
	}

	if (!cps_ncl_read_state(NULL)) {
		upslogx(LOG_WARNING,
			"CyberPower %s: unable to refresh NCL Bank 1 state",
			CPS_NCL_PRODUCT_NAME);
	}
}

int cps_ncl_instcmd(const char *cmdname, const char *extradata)
{
	const char *command;
	usb_ctrl_char response[CPS_NCL_PACKET_SIZE];
	size_t response_len = 0;
	int expected_state;
	int actual_state;
	int attempt;

	if (!cps_ncl_enabled) {
		return STAT_INSTCMD_UNKNOWN;
	}

	if (!strcasecmp(cmdname, "outlet.1.load.off")) {
		command = "S0:2\r";
		expected_state = 0;
	} else if (!strcasecmp(cmdname, "outlet.1.load.on")) {
		command = "W0:2\r";
		expected_state = 1;
	} else {
		return STAT_INSTCMD_UNKNOWN;
	}

	memset(response, 0, sizeof(response));
	if (!cps_ncl_command(command, response, sizeof(response), &response_len)) {
		upslog_INSTCMD_FAILED(cmdname, extradata);
		return STAT_INSTCMD_FAILED;
	}

	if (response_len != 3 || memcmp(response, "#0\r", 3) != 0) {
		upslogx(LOG_WARNING,
			"%s: CyberPower command '%s' returned an unexpected acknowledgement",
			__func__, cmdname);
		upslog_INSTCMD_FAILED(cmdname, extradata);
		return STAT_INSTCMD_FAILED;
	}

	/* ACK means that the command was accepted, not that the relay has
	 * finished switching. Verify the requested physical state. */
	for (attempt = 0; attempt < CPS_NCL_VERIFY_RETRIES; attempt++) {
		usleep(CPS_NCL_VERIFY_INTERVAL_US);
		if (!cps_ncl_read_state(&actual_state)) {
			continue;
		}

		if (actual_state == expected_state) {
			upsdebugx(2, "%s: %s physically verified", __func__, cmdname);
			return STAT_INSTCMD_HANDLED;
		}
	}

	upslogx(LOG_WARNING,
		"%s: command '%s' was acknowledged but requested NCL state was not observed",
		__func__, cmdname);
	upslog_INSTCMD_FAILED(cmdname, extradata);
	return STAT_INSTCMD_FAILED;
}
