/*
	usbmuxd - iPhone/iPod Touch USB multiplex server daemon

Copyright (C) 2009	Hector Martin "marcan" <hector@marcansoft.com>
Copyright (C) 2009	Nikias Bassen <nikias@gmx.li>

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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
	#include <WinSock2.h>
	#include <tchar.h>
#else
	#include <sys/types.h>

	#include <unistd.h>

	#include <sys/socket.h>
	#include <sys/un.h>
	#include <arpa/inet.h>
#endif

#include <pthread.h>

#include <plist/plist.h>

#include "log.h"
#include "usb.h"
#include "client.h"
#include "device.h"
#include "conf.h"

#define CMD_BUF_SIZE	0x10000
#define REPLY_BUF_SIZE	0x10000
#define CLIENT_SOCKET_BUFFERS_SIZE (0x10000)

#define LIBUSBMXD_PLIST_BUNDLE_ID ("org.libimobiledevice.usbmuxd")

enum client_state {
	CLIENT_COMMAND,		// waiting for command
	CLIENT_LISTEN,		// listening for devices
	CLIENT_CONNECTING1,	// issued connection request
	CLIENT_CONNECTING2,	// connection established, but waiting for response message to get sent
	CLIENT_CONNECTED,	// connected
	CLIENT_DEAD
};

struct mux_client {
	int fd;
	unsigned char *ob_buf;
	uint32_t ob_size;
	uint32_t ob_capacity;
	unsigned char *ib_buf;
	uint32_t ib_size;
	uint32_t ib_capacity;
	short events, devents;
	uint32_t connect_tag;
	int connect_device;
	enum client_state state;
	uint32_t proto_version;
};

static struct collection client_list;
pthread_mutex_t client_list_mutex;

/**
 * Receive raw data from the client socket.
 *
 * @param client Client to read from.
 * @param buffer Buffer to store incoming data.
 * @param len Max number of bytes to read.
 * @return Same as recv() system call. Number of bytes read; when < 0 errno will be set.
 */
int client_read(struct mux_client *client, void *buffer, uint32_t len)
{
	usbmuxd_log(LL_SPEW, "client_read fd %d buf %p len %d", client->fd, buffer, len);
	if(client->state != CLIENT_CONNECTED) {
		usbmuxd_log(LL_ERROR, "Attempted to read from client %d not in CONNECTED state", client->fd);
		return -1;
	}
	return recv(client->fd, (char *)buffer, len, 0);
}

/**
 * Send raw data to the client socket.
 *
 * @param client Client to send to.
 * @param buffer The data to send.
 * @param len Number of bytes to write.
 * @return Same as system call send(). Number of bytes written; when < 0 errno will be set.
 */
int client_write(struct mux_client *client, void *buffer, uint32_t len)
{
	int sret = -1;

	usbmuxd_log(LL_SPEW, "client_write fd %d buf %p len %d", client->fd, buffer, len);
	if(client->state != CLIENT_CONNECTED) {
		usbmuxd_log(LL_ERROR, "Attempted to write to client %d not in CONNECTED state", client->fd);
		return -1;
	}

	sret = send(client->fd, (const char *)buffer, len, 0);
	if (sret < 0) {
		if (WSAGetLastError() == WSAEWOULDBLOCK) {
			usbmuxd_log(LL_ERROR, "ERROR: client_write: fd %d not ready for writing", client->fd);
		} else {
			usbmuxd_log(LL_ERROR, "ERROR: client_write: sending to fd %d failed: %d", client->fd, WSAGetLastError());
		}
	}
	return sret;
}

/**
 * Set event mask to use for ppoll()ing the client socket.
 * Typically POLLOUT and/or POLLIN. Note that this overrides
 * the current mask, that is, it is not ORing the argument
 * into the current mask.
 *
 * @param client The client to set the event mask on.
 * @param events The event mask to sert.
 * @return 0 on success, -1 on error.
 */
int client_set_events(struct mux_client *client, short events)
{
	if((client->state != CLIENT_CONNECTED) && (client->state != CLIENT_CONNECTING2)) {
		usbmuxd_log(LL_ERROR, "client_set_events to client %d not in CONNECTED state", client->fd);
		return -1;
	}
	client->devents = events;
	if(client->state == CLIENT_CONNECTED)
		client->events = events;
	return 0;
}

/**
 * Wait for an inbound connection on the usbmuxd socket
 * and create a new mux_client instance for it, and store
 * the client in the client list.
 *
 * @param listenfd the socket fd to accept() on.
 * @param reject_connection should we accept the connection and close it (reject it)
 * @return The connection fd for the client, or < 0 for error
 *   in which case errno will be set.
 */
int client_accept(int listenfd, int reject_connection)
{
	struct sockaddr_in addr;
	int cfd;
	int len = sizeof(struct sockaddr_in);
	cfd = accept(listenfd, (struct sockaddr *)&addr, &len);
	if (cfd < 0) {
		usbmuxd_log(LL_ERROR, "accept() failed (%s)", strerror(errno));
		return cfd;
	}

	if (reject_connection) {
		usbmuxd_log(LL_WARNING, "rejecting new client connection");
		closesocket(cfd);
		return 0;
	}

	/* Set the new socket's buffer sizes */
	int socket_buf_size = CLIENT_SOCKET_BUFFERS_SIZE;
	if ((setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, (const char *)&socket_buf_size, sizeof(DWORD)) < 0) ||
		(setsockopt(cfd, SOL_SOCKET, SO_RCVBUF, (const char *)&socket_buf_size, sizeof(DWORD)) < 0))
	{
		usbmuxd_log(LL_ERROR, "setsockopt has failed");
		closesocket(cfd);
		return -1;
	}

	/* Switch the socket to a non-blocking mode */
	unsigned long non_blocking_mode = 1;
	if (ioctlsocket(cfd, FIONBIO, &non_blocking_mode) < 0)
	{
		usbmuxd_log(LL_ERROR, "ioctlsocket has failed");
		closesocket(cfd);
		return -1;
	}

	struct mux_client *client;
	client = (struct mux_client *)malloc(sizeof(struct mux_client));
	memset(client, 0, sizeof(struct mux_client));

	client->fd = cfd;
	client->ob_buf = (unsigned char *)malloc(REPLY_BUF_SIZE);
	client->ob_size = 0;
	client->ob_capacity = REPLY_BUF_SIZE;
	client->ib_buf = (unsigned char *)malloc(CMD_BUF_SIZE);
	client->ib_size = 0;
	client->ib_capacity = CMD_BUF_SIZE;
	client->state = CLIENT_COMMAND;
	client->events = POLLIN;

	pthread_mutex_lock(&client_list_mutex);
	collection_add(&client_list, client);
	pthread_mutex_unlock(&client_list_mutex);

	usbmuxd_log(LL_INFO, "New client on fd %d", client->fd);
	return client->fd;
}

void client_close(struct mux_client *client)
{
	usbmuxd_log(LL_INFO, "Disconnecting client fd %d", client->fd);
	if(client->state == CLIENT_CONNECTING1 || client->state == CLIENT_CONNECTING2) {
		usbmuxd_log(LL_INFO, "Client died mid-connect, aborting device %d connection", client->connect_device);
		client->state = CLIENT_DEAD;
		device_abort_connect(client->connect_device, client);
	}
	closesocket(client->fd);
	if(client->ob_buf)
		free(client->ob_buf);
	if(client->ib_buf)
		free(client->ib_buf);
	pthread_mutex_lock(&client_list_mutex);
	collection_remove(&client_list, client);
	pthread_mutex_unlock(&client_list_mutex);
	free(client);
}

int client_add_fds(fd_set * read_fds, fd_set * write_fds)
{
	int clients_count = 0;
	pthread_mutex_lock(&client_list_mutex);
	FOREACH(struct mux_client *client, &client_list, struct mux_client *)
	{
		if ((client->events) & POLLIN)
		{
			FD_SET(client->fd, read_fds);
		}
		if ((client->events) & POLLOUT)
		{
			FD_SET(client->fd, write_fds);
		}

		/* To make sure we have enough available fds to use, we'll assume all client 
		 * have data */
		clients_count++;
	} ENDFOREACH
	pthread_mutex_unlock(&client_list_mutex);

	return clients_count;
}

static int send_pkt(struct mux_client *client, uint32_t tag, enum usbmuxd_msgtype msg, void *payload, int payload_length)
{
	struct usbmuxd_header hdr;
	hdr.version = client->proto_version;
	hdr.length = sizeof(hdr) + payload_length;
	hdr.message = msg;
	hdr.tag = tag;
	usbmuxd_log(LL_DEBUG, "send_pkt fd %d tag %d msg %d payload_length %d", client->fd, tag, msg, payload_length);

	uint32_t available = client->ob_capacity - client->ob_size;
	/* the output buffer _should_ be large enough, but just in case */
	if(available < hdr.length) {
		unsigned char* new_buf;
		uint32_t new_size = ((client->ob_capacity + hdr.length + 4096) / 4096) * 4096;
		usbmuxd_log(LL_DEBUG, "%s: Enlarging client %d output buffer %d -> %d", __func__, client->fd, client->ob_capacity, new_size);
		new_buf = (unsigned char *)realloc(client->ob_buf, new_size);
		if (!new_buf) {
			usbmuxd_log(LL_FATAL, "%s: Failed to realloc.\n", __func__);
			return -1;
		}
		client->ob_buf = new_buf;
		client->ob_capacity = new_size;
	}
	memcpy(client->ob_buf + client->ob_size, &hdr, sizeof(hdr));
	if(payload && payload_length)
		memcpy(client->ob_buf + client->ob_size + sizeof(hdr), payload, payload_length);
	client->ob_size += hdr.length;
	client->events |= POLLOUT;
	return hdr.length;
}

static int send_plist_pkt(struct mux_client *client, uint32_t tag, plist_t plist)
{
	int res = -1;
	char *xml = NULL;
	uint32_t xmlsize = 0;
	plist_to_xml(plist, &xml, &xmlsize);
	if (xml) {
		res = send_pkt(client, tag, MESSAGE_PLIST, xml, xmlsize);
		plist_free_memory(xml);
	} else {
		usbmuxd_log(LL_ERROR, "%s: Could not convert plist to xml", __func__);
	}
	return res;
}

static int send_result(struct mux_client *client, uint32_t tag, uint32_t result)
{
	int res = -1;
	if (client->proto_version == 1) {
		/* XML plist packet */
		plist_t dict = plist_new_dict();
		plist_dict_set_item(dict, "MessageType", plist_new_string("Result"));
		plist_dict_set_item(dict, "Number", plist_new_uint(result));
		res = send_plist_pkt(client, tag, dict);
		plist_free(dict);
	} else {
		/* binary packet */
		res = send_pkt(client, tag, MESSAGE_RESULT, &result, sizeof(uint32_t));
	}
	return res;
}

int client_notify_connect(struct mux_client *client, enum usbmuxd_result result)
{
	usbmuxd_log(LL_SPEW, "client_notify_connect fd %d result %d", client->fd, result);
	if(client->state == CLIENT_DEAD)
		return -1;
	if(client->state != CLIENT_CONNECTING1) {
		usbmuxd_log(LL_ERROR, "client_notify_connect when client %d is not in CONNECTING1 state", client->fd);
		return -1;
	}
	if(send_result(client, client->connect_tag, result) < 0)
		return -1;
	if(result == RESULT_OK) {
		client->state = CLIENT_CONNECTING2;
		client->events = POLLOUT; // wait for the result packet to go through
		// no longer need this
		free(client->ib_buf);
		client->ib_buf = NULL;
	} else {
		client->state = CLIENT_COMMAND;
	}
	return 0;
}

static plist_t create_device_attached_plist(struct device_info *dev)
{
	plist_t dict = plist_new_dict();
	plist_dict_set_item(dict, "MessageType", plist_new_string("Attached"));
	plist_dict_set_item(dict, "DeviceID", plist_new_uint(dev->id));
	plist_t props = plist_new_dict();
	// TODO: get current usb speed
	plist_dict_set_item(props, "ConnectionSpeed", plist_new_uint(480000000));
	plist_dict_set_item(props, "ConnectionType", plist_new_string("USB"));
	plist_dict_set_item(props, "DeviceID", plist_new_uint(dev->id));
	plist_dict_set_item(props, "LocationID", plist_new_uint(dev->location));
	plist_dict_set_item(props, "ProductID", plist_new_uint(dev->pid));
	plist_dict_set_item(props, "SerialNumber", plist_new_string(dev->serial));
	plist_dict_set_item(dict, "Properties", props);
	return dict;
}

static plist_t create_device_pairing_event_plist(struct device_info *dev, const char * event)
{
	plist_t dict = plist_new_dict();
	plist_dict_set_item(dict, "MessageType", plist_new_string(event));
	plist_dict_set_item(dict, "DeviceID", plist_new_uint(dev->id));
	plist_t props = plist_new_dict();
	plist_dict_set_item(props, "DeviceID", plist_new_uint(dev->id));
	plist_dict_set_item(props, "LocationID", plist_new_uint(dev->location));
	plist_dict_set_item(props, "ProductID", plist_new_uint(dev->pid));
	plist_dict_set_item(props, "SerialNumber", plist_new_string(dev->serial ? dev->serial : ""));
	plist_dict_set_item(dict, "Properties", props);
	return dict;
}

static int send_device_list(struct mux_client *client, uint32_t tag)
{
	int res = -1;
	plist_t dict = plist_new_dict();
	plist_t devices = plist_new_array();

	struct device_info *devs = NULL;
	struct device_info *dev;
	int i;
	
	///////////////////////////////////////////////////////
	// add support for MCE_INCLUDE_HIDDEN_DEVICES env var
	int include_hidden = 0;
	TCHAR includeHiddenVar[0x40] = { 0 };
	if (GetEnvironmentVariable(TEXT("MCE_INCLUDE_HIDDEN_DEVICES"), includeHiddenVar, sizeof(includeHiddenVar) / sizeof(TCHAR))){
		if (0 == _tcsicmp(includeHiddenVar, TEXT("true"))) {
			include_hidden = 1;
		}
	}
	///////////////////////////////////////////////////////

	int count = device_get_list(include_hidden, &devs);
	dev = devs;
	for (i = 0; devs && i < count; i++) {
		plist_t device = create_device_attached_plist(dev++);
		if (device) {
			plist_array_append_item(devices, device);
		}
	}
	if (devs)
		free(devs);

	plist_dict_set_item(dict, "DeviceList", devices);
	res = send_plist_pkt(client, tag, dict);
	plist_free(dict);
	return res;
}

static int send_system_buid(struct mux_client *client, uint32_t tag)
{
	int res = -1;
	char* buid = NULL;

	config_get_system_buid(&buid);

	plist_t dict = plist_new_dict();
	plist_dict_set_item(dict, "BUID", plist_new_string(buid));
	res = send_plist_pkt(client, tag, dict);
	plist_free(dict);
	return res;
}

static int send_pair_record(struct mux_client *client, uint32_t tag, const char* record_id)
{
	int res = -1;
	char* record_data = NULL;
	uint64_t record_size = 0;

	if (!record_id) {
		return send_result(client, tag, EINVAL);
	}

	config_get_device_record(record_id, &record_data, &record_size);
	
	if (record_data) {
		plist_t pair_record_dict = NULL;
		plist_from_xml(record_data, record_size, &pair_record_dict);
		if (pair_record_dict) {
			if ((NULL == plist_dict_get_item(pair_record_dict, "DeviceCertificate")) ||
				(NULL == plist_dict_get_item(pair_record_dict, "HostID")) ||
				(NULL == plist_dict_get_item(pair_record_dict, "SystemBUID")))
			{
				usbmuxd_log(LL_ERROR, "Partial pairing record");
				plist_free(pair_record_dict);
				free(record_data);
				return send_result(client, tag, ENOENT);
			}

			plist_free(pair_record_dict);
		}
	}
	
	if (record_data) {
		plist_t dict = plist_new_dict();
		plist_dict_set_item(dict, "PairRecordData", plist_new_data(record_data, record_size));
		free(record_data);
		res = send_plist_pkt(client, tag, dict);
		plist_free(dict);
	} else {
		res = send_result(client, tag, ENOENT);
	}
	return res;
}

static int notify_device_add(struct mux_client *client, struct device_info *dev)
{
	int res = -1;
	if (client->proto_version == 1) {
		/* XML plist packet */
		plist_t dict = create_device_attached_plist(dev);
		res = send_plist_pkt(client, 0, dict);
		plist_free(dict);
	} else {
		/* binary packet */
		struct usbmuxd_device_record dmsg;
		memset(&dmsg, 0, sizeof(dmsg));
		dmsg.device_id = dev->id;
		strncpy(dmsg.serial_number, dev->serial, 256);
		dmsg.serial_number[255] = 0;
		dmsg.location = dev->location;
		dmsg.product_id = dev->pid;
		res = send_pkt(client, 0, MESSAGE_DEVICE_ADD, &dmsg, sizeof(dmsg));
	}
	return res;
}

static int notify_device_remove(struct mux_client *client, uint32_t device_id)
{
	int res = -1;
	if (client->proto_version == 1) {
		/* XML plist packet */
		plist_t dict = plist_new_dict();
		plist_dict_set_item(dict, "MessageType", plist_new_string("Detached"));
		plist_dict_set_item(dict, "DeviceID", plist_new_uint(device_id));
		res = send_plist_pkt(client, 0, dict);
		plist_free(dict);
	} else {
		/* binary packet */
		res = send_pkt(client, 0, MESSAGE_DEVICE_REMOVE, &device_id, sizeof(uint32_t));
	}
	return res;
}

static int start_listen(struct mux_client *client)
{
	struct device_info *devs = NULL;
	struct device_info *dev;
	int count, i;

	client->state = CLIENT_LISTEN;

	count = device_get_list(0, &devs);
	dev = devs;
	for(i=0; devs && i < count; i++) {
		if(notify_device_add(client, dev++) < 0) {
			free(devs);
			return -1;
		}
	}
	if (devs)
		free(devs);

	return count;
}

static char* plist_dict_get_string_val(plist_t dict, const char* key)
{
	if (!dict || plist_get_node_type(dict) != PLIST_DICT)
		return NULL;
	plist_t item = plist_dict_get_item(dict, key);
	if (!item || plist_get_node_type(item) != PLIST_STRING)
		return NULL;
	char *str = NULL;
	plist_get_string_val(item, &str);
	return str;
}

static int handle_save_pair_record_command(struct mux_client *client, struct usbmuxd_header *hdr, plist_t command_dict)
{
	int res = -1;
	uint32_t rval = RESULT_OK;
					
	/* Get the client's bundle id */
	char *bundle_id = NULL;
	plist_t bundle_id_node = plist_dict_get_item(command_dict, "BundleID");
	if (bundle_id_node) {
		plist_get_string_val(bundle_id_node, &bundle_id);
	}

	/* Only allow libusbmuxd to save pair records */
	if (bundle_id && (0 == strcmp(bundle_id, LIBUSBMXD_PLIST_BUNDLE_ID))) {
		char* record_id = plist_dict_get_string_val(command_dict, "PairRecordID");
		char* record_data = NULL;
		uint64_t record_size = 0;
		plist_t rdata = plist_dict_get_item(command_dict, "PairRecordData");
		if (rdata && plist_get_node_type(rdata) == PLIST_DATA) {
			plist_get_data_val(rdata, &record_data, &record_size);
		}

		if (record_id && record_data) {
			res = config_set_device_record(record_id, record_data, record_size);
			if (res < 0) {
				rval = -res;
			}
			plist_free_memory(record_id);
		} else {
			rval = EINVAL;
		}
	} else {
		usbmuxd_log(LL_INFO, "Ignoring SavePairRecord message");
	}
					
	if (bundle_id) {
		plist_free_memory(bundle_id);
	}

	if (send_result(client, hdr->tag, rval) < 0)
		return -1;
	return 0;
}

static int handle_connect_command(struct mux_client *client, struct usbmuxd_header *hdr, plist_t command_dict)
{
	int res = -1;
	uint64_t val;
	uint16_t portnum = 0;
	uint32_t device_id = 0;

	// get device id
	plist_t node = plist_dict_get_item(command_dict, "DeviceID");
	if (!node) {
		usbmuxd_log(LL_ERROR, "Received connect request without device_id!");
		if (send_result(client, hdr->tag, RESULT_BADDEV) < 0)
			return -1;
		return 0;
	}
	val = 0;
	plist_get_uint_val(node, &val);
	device_id = (uint32_t)val;

	// get port number
	node = plist_dict_get_item(command_dict, "PortNumber");
	if (!node) {
		usbmuxd_log(LL_ERROR, "Received connect request without port number!");
		if (send_result(client, hdr->tag, RESULT_BADCOMMAND) < 0)
			return -1;
		return 0;
	}
	val = 0;
	plist_get_uint_val(node, &val);
	portnum = (uint16_t)val;

	usbmuxd_log(LL_DEBUG, "Client %d connection request to device %d port %d", client->fd, device_id, ntohs(portnum));
	res = device_start_connect(device_id, ntohs(portnum), client);
	if(res < 0) {
		if (send_result(client, hdr->tag, -res) < 0)
			return -1;
	} else {
		client->connect_tag = hdr->tag;
		client->connect_device = device_id;
		client->state = CLIENT_CONNECTING1;
	}
	return 0;
}

static int handle_read_pair_record_command(struct mux_client *client, struct usbmuxd_header *hdr, plist_t command_dict)
{
	char* record_id = plist_dict_get_string_val(command_dict, "PairRecordID");

	//////////////////////////////////////////////////////
	// handle "-" in udid
	if (char* pos = strstr(record_id, "-")){
		for (; pos[0]; pos++){
			pos[0] = pos[1];
		}
	}
	//////////////////////////////////////////////////////

	int res = send_pair_record(client, hdr->tag, record_id);
	if (record_id)
		plist_free_memory(record_id);
	if (res < 0)
		return -1;
	return 0;
}

static int handle_delete_pair_record_command(struct mux_client *client, struct usbmuxd_header *hdr, plist_t command_dict)
{
	uint32_t rval = RESULT_OK;
	char* record_id = plist_dict_get_string_val(command_dict, "PairRecordID");
	if (record_id) {
		int res = config_remove_device_record(record_id);
		if (res < 0) {
			rval = -res;
		}
		plist_free_memory(record_id);
	} else {
		rval = EINVAL;
	}
	if (send_result(client, hdr->tag, rval) < 0)
		return -1;
	return 0;
}

static int handle_listen_command(struct mux_client *client, struct usbmuxd_header *hdr)
{
	if (send_result(client, hdr->tag, 0) < 0)
		return -1;
	
	usbmuxd_log(LL_DEBUG, "Client %d now LISTENING", client->fd);
	return start_listen(client);
}

static uint32_t get_device_location_from_device_command(plist_t command_dict)
{
	plist_t location_node = plist_dict_get_item(command_dict, "DeviceLocation");
	if (!location_node || (plist_get_node_type(location_node) != PLIST_UINT)) {
		usbmuxd_log(LL_ERROR, "Missing DeviceLocation from the DeviceMonitor request");
		return 0;
	}
		
	uint64_t location = 0;
	plist_get_uint_val(location_node, &location);
	
	return (uint32_t)location;
}

static int handle_add_device_command(struct mux_client *client, struct usbmuxd_header *hdr, plist_t command_dict)
{
	/* Get the device location */
	uint32_t device_location = get_device_location_from_device_command(command_dict);
	if (!device_location) {
		(void)send_result(client, hdr->tag, RESULT_BADCOMMAND);
		return -1;
	}

	/* Add the device */
	if (usb_add_device(device_location, NULL) < 0) {
		usbmuxd_log(LL_ERROR, "Failed to add %s", device_location);
		(void)send_result(client, hdr->tag, -EIO);
		return -1;
	}

	if (send_result(client, hdr->tag, RESULT_OK) < 0) {
		return -1;
	}
	return 0;
}

static int handle_remove_device_command(struct mux_client *client, struct usbmuxd_header *hdr, plist_t command_dict)
{
	/* Get the device location */
	uint32_t device_location = get_device_location_from_device_command(command_dict);
	if (!device_location) {
		(void)send_result(client, hdr->tag, RESULT_BADCOMMAND);
		return -1;
	}

	/* remove the device */
	if (usb_remove_device(device_location, NULL) < 0) {
		usbmuxd_log(LL_ERROR, "Failed to remove %s", device_location);
		(void)send_result(client, hdr->tag, -EIO);
		return -1;
	}

	if (send_result(client, hdr->tag, RESULT_OK) < 0) {
		return -1;
	}
	return 0;
}

static int handle_device_monitor_command(struct mux_client *client, struct usbmuxd_header *hdr, plist_t command_dict)
{
	/* Get the device location */
	uint32_t device_location = get_device_location_from_device_command(command_dict);
	if (!device_location) {
		(void)send_result(client, hdr->tag, RESULT_BADCOMMAND);
		return -1;
	}

	/* Get the auto-monitor value */
	uint8_t auto_monitor = 0;
	plist_t auto_monitor_node = plist_dict_get_item(command_dict, "AutoMonitor");
	if (auto_monitor_node && (PLIST_BOOLEAN == plist_get_node_type(auto_monitor_node))) {
		plist_get_bool_val(auto_monitor_node, &auto_monitor);
	}

	/* Set the device's monitoring state */
	device_monitor_state monitor_state = auto_monitor ? DEVICE_MONITOR_ALWAYS : DEVICE_MONITOR_DISABLE;
	if (usb_set_device_monitoring_immediately(device_location, monitor_state) < 0) {
		usbmuxd_log(LL_ERROR, "Failed to set %d monitoring state", device_location);
		(void)send_result(client, hdr->tag, -EIO);
		return -1;
	}

	if (send_result(client, hdr->tag, RESULT_OK) < 0) {
		return -1;
	}
	return 0;
}

static int handle_client_plist_command(struct mux_client *client, struct usbmuxd_header *hdr)
{
	int res = -1;
	plist_t command_dict = NULL;

	/* Get the packet's payload */
	client->proto_version = 1;
	char * payload = (char*)(hdr)+sizeof(struct usbmuxd_header);
	uint32_t payload_size = hdr->length - sizeof(struct usbmuxd_header);

	plist_from_xml(payload, payload_size, &command_dict);
	if (!command_dict) {
		usbmuxd_log(LL_ERROR, "Could not parse plist from payload!");
		return -1;
	} 
	
	char *message = NULL;
	plist_t node = plist_dict_get_item(command_dict, "MessageType");
	plist_get_string_val(node, &message);
	if (!message) {
		usbmuxd_log(LL_ERROR, "Could not extract MessageType from plist!");
		plist_free(command_dict);
		return -1;
	}

	/* Listen */
	if (!strcmp(message, "Listen")) {
		res = handle_listen_command(client, hdr);

	/* Connect */
	} else if (!strcmp(message, "Connect")) {
		res = handle_connect_command(client, hdr, command_dict);

	/* ListDevices */
	} else if (!strcmp(message, "ListDevices")) {
		res = send_device_list(client, hdr->tag) < 0 ? -1 : 0;

	/* ReadBUID */
	} else if (!strcmp(message, "ReadBUID")) {
		res = send_system_buid(client, hdr->tag) < 0 ? -1 : 0;

	/* ReadPairRecord */
	} else if (!strcmp(message, "ReadPairRecord")) {
		res = handle_read_pair_record_command(client, hdr, command_dict);

	/* SavePairRecord */
	} else if (!strcmp(message, "SavePairRecord")) {
		res = handle_save_pair_record_command(client, hdr, command_dict);

	/* DeletePairRecord */
	} else if (!strcmp(message, "DeletePairRecord")) {
		res = handle_delete_pair_record_command(client, hdr, command_dict);
	
	/* AddDevice */
	} else if (!strcmp(message, "AddDevice")) {
		res = handle_add_device_command(client, hdr, command_dict);

	/* RemoveDevice */
	} else if (!strcmp(message, "RemoveDevice")) {
		res = handle_remove_device_command(client, hdr, command_dict);

	/* DeviceMonitor */
	} else if (!strcmp(message, "DeviceMonitor")) {
		res = handle_device_monitor_command(client, hdr, command_dict);

	/* Unknown */
	} else {
		usbmuxd_log(LL_ERROR, "Unexpected command '%s' received!", message);
		res = send_result(client, hdr->tag, RESULT_BADCOMMAND) < 0 ? -1 : 0;
	}

	/* Cleanup */
	plist_free_memory(message);
	plist_free(command_dict);

	return res;
}

static int client_command(struct mux_client *client, struct usbmuxd_header *hdr)
{
	int res;
	usbmuxd_log(LL_DEBUG, "Client command in fd %d len %d ver %d msg %d tag %d", client->fd, hdr->length, hdr->version, hdr->message, hdr->tag);

	if(client->state != CLIENT_COMMAND) {
		usbmuxd_log(LL_ERROR, "Client %d command received in the wrong state", client->fd);
		if(send_result(client, hdr->tag, RESULT_BADCOMMAND) < 0)
			return -1;
		client_close(client);
		return -1;
	}

	if((hdr->version != 0) && (hdr->version != 1)) {
		usbmuxd_log(LL_INFO, "Client %d version mismatch: expected 0 or 1, got %d", client->fd, hdr->version);
		send_result(client, hdr->tag, RESULT_BADVERSION);
		return 0;
	}

	struct usbmuxd_connect_request *ch;

	switch(hdr->message) {
		case MESSAGE_PLIST:
			return handle_client_plist_command(client, hdr);
		case MESSAGE_LISTEN:
			if(send_result(client, hdr->tag, 0) < 0)
				return -1;
			usbmuxd_log(LL_DEBUG, "Client %d now LISTENING", client->fd);
			return start_listen(client);
		case MESSAGE_CONNECT:
			ch = (struct usbmuxd_connect_request *)hdr;
			usbmuxd_log(LL_DEBUG, "Client %d connection request to device %d port %d", client->fd, ch->device_id, ntohs(ch->port));
			res = device_start_connect(ch->device_id, ntohs(ch->port), client);
			if(res < 0) {
				if(send_result(client, hdr->tag, -res) < 0)
					return -1;
			} else {
				client->connect_tag = hdr->tag;
				client->connect_device = ch->device_id;
				client->state = CLIENT_CONNECTING1;
			}
			return 0;
		default:
			usbmuxd_log(LL_ERROR, "Client %d invalid command %d", client->fd, hdr->message);
			if(send_result(client, hdr->tag, RESULT_BADCOMMAND) < 0)
				return -1;
			return 0;
	}
	return -1;
}

static void process_send(struct mux_client *client)
{
	int res;
	if(!client->ob_size) {
		usbmuxd_log(LL_WARNING, "Client %d OUT process but nothing to send?", client->fd);
		client->events &= ~POLLOUT;
		return;
	}
	res = send(client->fd, (const char *)(client->ob_buf), client->ob_size, 0);
	if(res <= 0) {
		usbmuxd_log(LL_ERROR, "Send to client fd %d failed: %d %s", client->fd, res, strerror(errno));
		client_close(client);
		return;
	}
	if((uint32_t)res == client->ob_size) {
		client->ob_size = 0;
		client->events &= ~POLLOUT;
		if(client->state == CLIENT_CONNECTING2) {
			usbmuxd_log(LL_DEBUG, "Client %d switching to CONNECTED state", client->fd);
			client->state = CLIENT_CONNECTED;
			client->events = client->devents;
			// no longer need this
			free(client->ob_buf);
			client->ob_buf = NULL;
		}
	} else {
		client->ob_size -= res;
		memmove(client->ob_buf, client->ob_buf + res, client->ob_size);
	}
}
static void process_recv(struct mux_client *client)
{
	int res;
	int did_read = 0;
	if(client->ib_size < sizeof(struct usbmuxd_header)) {
		res = recv(client->fd, (char *)(client->ib_buf) + client->ib_size, sizeof(struct usbmuxd_header) - client->ib_size, 0);
		if(res <= 0) {
			usbmuxd_log(LL_ERROR, "DBG: RECV client fd %d failed: %u", client->fd, WSAGetLastError());
			
			if(res < 0)
				usbmuxd_log(LL_ERROR, "Receive from client fd %d failed: %s", client->fd, strerror(errno));
			else
				usbmuxd_log(LL_INFO, "Client %d connection closed", client->fd);
			client_close(client);
			return;
		}
		client->ib_size += res;
		if(client->ib_size < sizeof(struct usbmuxd_header))
			return;
		did_read = 1;
	}
	struct usbmuxd_header *hdr = (struct usbmuxd_header *)client->ib_buf;
	if(hdr->length > client->ib_capacity) {
		usbmuxd_log(LL_INFO, "Client %d message is too long (%d bytes)", client->fd, hdr->length);
		client_close(client);
		return;
	}
	if(hdr->length < sizeof(struct usbmuxd_header)) {
		usbmuxd_log(LL_ERROR, "Client %d message is too short (%d bytes)", client->fd, hdr->length);
		client_close(client);
		return;
	}
	if(client->ib_size < hdr->length) {
		if(did_read)
			return; //maybe we would block, so defer to next loop
		res = recv(client->fd, (char *)(client->ib_buf) + client->ib_size, hdr->length - client->ib_size, 0);
		if(res < 0) {
			usbmuxd_log(LL_ERROR, "Receive from client fd %d failed: %s", client->fd, strerror(errno));
			client_close(client);
			return;
		} else if(res == 0) {
			usbmuxd_log(LL_INFO, "Client %d connection closed", client->fd);
			client_close(client);
			return;
		}
		client->ib_size += res;
		if(client->ib_size < hdr->length)
			return;
	}
	client_command(client, hdr);
	client->ib_size = 0;
}

int client_process(fd_set * read_fds, fd_set * write_fds)
{
	pthread_mutex_lock(&client_list_mutex);
	FOREACH(struct mux_client *client, &client_list, struct mux_client *)
	{
		short events = 0;
		if (FD_ISSET(client->fd, read_fds))
		{
			events |= POLLIN;
		}
		if (FD_ISSET(client->fd, write_fds))
		{
			events |= POLLOUT;
		}

		if (events)
		{
			if (client->state == CLIENT_CONNECTED)
			{
				usbmuxd_log(LL_SPEW, "client_process in CONNECTED state");
				device_client_process(client->connect_device, client, events);
			}
			else
			{
				if (events & POLLIN)
				{
					process_recv(client);
				}
				else if (events & POLLOUT)
				{
					//not both in case client died as part of process_recv
					process_send(client);
				}
			}
		}
	} ENDFOREACH
	pthread_mutex_unlock(&client_list_mutex);

	return 0;
}

void client_device_add(struct device_info *dev)
{
	device_set_visible(dev->id);

	pthread_mutex_lock(&client_list_mutex);
	usbmuxd_log(LL_DEBUG, "client_device_add: id %d, location 0x%x, serial %s", dev->id, dev->location, dev->serial);
	
	FOREACH(struct mux_client *client, &client_list, struct mux_client *)
	{
		if(client->state == CLIENT_LISTEN)
			notify_device_add(client, dev);
	} ENDFOREACH
	pthread_mutex_unlock(&client_list_mutex);
}

void client_device_remove(int device_id)
{
	pthread_mutex_lock(&client_list_mutex);
	uint32_t id = device_id;
	usbmuxd_log(LL_DEBUG, "client_device_remove: id %d", device_id);
	FOREACH(struct mux_client *client, &client_list, struct mux_client *)
	{
		if(client->state == CLIENT_LISTEN)
			notify_device_remove(client, id);
	} ENDFOREACH
	pthread_mutex_unlock(&client_list_mutex);
}

static void client_device_pairing_event(struct device_info *dev, const char * event, int validate_device)
{
	/* Lock the devices list, to make sure the device exists while we send
	 * an event about it */
	device_lock_devices();
	
	if (!validate_device || device_exists(dev->id, 0)) {
		pthread_mutex_lock(&client_list_mutex);
		usbmuxd_log(LL_DEBUG, "client_device_pairing_event: id %d, location 0x%x, serial %s, event - %s", dev->id, dev->location, dev->serial, event);
		FOREACH(struct mux_client *client, &client_list, struct mux_client *)
		{
			/* We only support XML plist */
			if ((client->state == CLIENT_LISTEN) && (client->proto_version == 1)) {
				plist_t dict = create_device_pairing_event_plist(dev, event);
				if (send_plist_pkt(client, 0, dict) < 0) {
					usbmuxd_log(LL_ERROR, "client_device_pairing_event: Failed to send plist");
				}
				plist_free(dict);
			}
		} ENDFOREACH
		pthread_mutex_unlock(&client_list_mutex);	
	} else {
		usbmuxd_log(LL_DEBUG, "client_device_pairing_event: Device id %d, location 0x%x, was removed - ignoring %s event", dev->id, dev->location, event); \
	}
	
	device_unlock_devices();
}

void client_device_removed_during_add(struct device_info *dev)
{
	/* Don't check if the device exists, since we might have failed before device_add
	 * was called */
	client_device_pairing_event(dev, "RemovedDuringAdd", 0);
}

void client_device_trust_pending(struct device_info *dev)
{
	client_device_pairing_event(dev, "TrustPending", 1);
}

void client_device_password_protected(struct device_info *dev)
{
	client_device_pairing_event(dev, "PasswordProtected", 1);
}

void client_device_user_denied_pairing(struct device_info *dev)
{
	client_device_pairing_event(dev, "UserDeniedPairing", 1);
}

void client_device_error_already_exits(struct device_info *dev)
{
	client_device_pairing_event(dev, "ErrorDeviceAlreadyExists", 0);
}

void client_init(void)
{
	usbmuxd_log(LL_DEBUG, "client_init");
	collection_init(&client_list);
	pthread_mutex_init(&client_list_mutex, NULL);
}

void client_shutdown(void)
{
	usbmuxd_log(LL_DEBUG, "client_shutdown");
	FOREACH(struct mux_client *client, &client_list, struct mux_client *) {
		client_close(client);
	} ENDFOREACH
	pthread_mutex_destroy(&client_list_mutex);
	collection_free(&client_list);
}
