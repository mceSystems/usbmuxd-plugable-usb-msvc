/*
	usbmuxd - iPhone/iPod Touch USB multiplex server daemon

Copyright (C) 2009	Hector Martin "marcan" <hector@marcansoft.com>
Copyright (C) 2009	Nikias Bassen <nikias@gmx.li>
Copyright (C) 2009	Martin Szulecki <opensuse@sukimashita.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 or version 3.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

*/

#ifndef __USB_H__
#define __USB_H__

#include <stdint.h>
#include "utils.h"

#define INTERFACE_CLASS 255
#define INTERFACE_SUBCLASS 254
#define INTERFACE_PROTOCOL 2

// libusb fragments packets larger than this (usbfs limitation)
// on input, this creates race conditions and other issues
#define USB_MRU 16384

// max transmission packet size
// libusb fragments these too, but doesn't send ZLPs so we're safe
// but we need to send a ZLP ourselves at the end (see usb-linux.c)
// we're using 3 * 16384 to optimize for the fragmentation
// this results in three URBs per full transfer, 32 USB packets each
// if there are ZLP issues this should make them show up easily too
#define USB_MTU (3 * 16384)

#define USB_PACKET_SIZE 512

#define VID_APPLE 0x5ac
#define PID_RANGE_LOW 0x1290
#define PID_RANGE_MAX 0x12af

struct usb_device;

enum device_monitor_state
{
	DEVICE_MONITOR_ONCE,
	DEVICE_MONITOR_ALWAYS,
	DEVICE_MONITOR_DISABLE
};

int usb_init(uint32_t hub_address, LPCWSTR pLuginPath);
void usb_shutdown(void);
int usb_send(struct usb_device *dev, const unsigned char *buf, int length);
int usb_add_device(uint32_t device_location, void * completion_event);
int usb_remove_device(uint32_t device_location, void * completion_event);
usb_device * usb_get_device_by_id(int id);

const char *usb_get_serial(struct usb_device *dev);
uint32_t usb_get_location(struct usb_device *dev);
uint16_t usb_get_pid(struct usb_device *dev);
void usb_set_device_ready(struct usb_device *dev);

int usb_set_device_monitoring(uint32_t device_location, enum device_monitor_state monitor_device, uint32_t timeout);
int usb_set_device_monitoring_immediately(uint32_t device_location, enum device_monitor_state monitor_device);
uint8_t usb_has_devices();
int usb_is_device_monitored(struct usb_device *dev);

#ifdef USE_PORTDRIVER_SOCKETS
	int usb_process(fd_set * read_fds);
	int usb_add_fds(fd_set * read_fds, fd_set * write_fds);
#else
	int usb_start_read(struct usb_device * dev);
	int usb_get_read_result(struct usb_device * dev, void ** buf, uint32_t * buf_data_size);
	int usb_process(void * unsused);
#endif

#endif
