/*
	usbmuxd - iPhone/iPod Touch USB multiplex server daemon

Copyright (C) 2009	Hector Martin "marcan" <hector@marcansoft.com>

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

#ifndef __DEVICE_H__
#define __DEVICE_H__

#include "usb.h"
#include "client.h"

struct device_info {
	int id;
	const char *serial;
	uint32_t location;
	uint16_t pid;
};

#ifndef USE_PORTDRIVER_SOCKETS
	int device_accept_socket(int listenfd, int reject_connection);
	int device_add_fds(fd_set * read_fds, fd_set * write_fds);
	int device_process_sockets(fd_set * read_fds, fd_set * write_fds);
#endif

uint32_t device_data_input(struct usb_device *dev, unsigned char *buf, uint32_t length);

int device_add(struct usb_device *dev);
void device_remove(struct usb_device *dev);
void device_add_failed(struct usb_device *dev);

void device_lock_devices();
void device_unlock_devices();
int device_exists(int device_id, int lock_devices);

int device_start_connect(int device_id, uint16_t port, struct mux_client *client);
void device_client_process(int device_id, struct mux_client *client, short events);
void device_abort_connect(int device_id, struct mux_client *client);

void device_set_visible(int device_id);
void device_set_preflight_cb_data(int device_id, void* data);

int device_get_count(int include_hidden);
int device_get_list(int include_hidden, struct device_info **devices);

int device_get_timeout(void);
void device_check_timeouts(void);

void device_init(void);
void device_kill_connections(void);
void device_shutdown(void);

int device_is_initializing(struct usb_device *usb_dev);
void device_preflight_finished(int device_id);

#endif
