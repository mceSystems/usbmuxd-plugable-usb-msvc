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

#define _BSD_SOURCE

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef _MSC_VER
	#include <cstdint>
	typedef uint32_t u_int32_t;
	#include <WinSock2.h>
#else
	#include <sys/time.h>
	#include <netinet/in.h>
#endif

#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <pthread.h>
#include "device.h"
#include "client.h"
#include "preflight.h"
#include "usb.h"
#include "log.h"

static int next_device_id;

#define DEV_MRU 65536

#define CONN_INBUF_SIZE		262144
#define CONN_OUTBUF_SIZE	65536

#define ACK_TIMEOUT 30

/* Max mux packet size (used to calculate max_payload).
 * Value was taken from iTunes, original value was USB_MTU */
#define MAX_MUX_PACKET_SIZE (0x7FFC)

enum mux_protocol {
	MUX_PROTO_VERSION = 0,
	MUX_PROTO_CONTROL = 1,
	MUX_PROTO_SETUP = 2,
	MUX_PROTO_TCP = IPPROTO_TCP,
};

enum mux_dev_state {
	MUXDEV_INIT,	// sent version packet
	MUXDEV_ACTIVE,	// received version packet, active
	MUXDEV_DEAD		// dead
};

enum mux_conn_state {
	CONN_CONNECTING,	// SYN
	CONN_CONNECTED,		// SYN/SYNACK/ACK -> active
	CONN_REFUSED,		// RST received during SYN
	CONN_DYING,			// RST received
	CONN_DEAD			// being freed; used to prevent infinite recursion between client<->device freeing
};

struct mux_header
{
	uint32_t protocol;
	uint32_t length;
	uint32_t magic;
	uint16_t tx_seq;
	uint16_t rx_seq;
};

struct version_header
{
	uint32_t major;
	uint32_t minor;
	uint32_t padding;
};

struct mux_device;

#define CONN_ACK_PENDING 1

struct mux_connection
{
	struct mux_device *dev;
	struct mux_client *client;
	enum mux_conn_state state;
	uint16_t sport, dport;
	uint32_t tx_seq, tx_ack, tx_acked, tx_win;
	uint32_t rx_seq, rx_ack, rx_win;
	uint32_t max_payload;
	uint32_t sendable;
	int flags;
	unsigned char *ib_buf;
	uint32_t ib_size;
	uint32_t ib_capacity;
	unsigned char *ob_buf;
	uint32_t ob_capacity;
	short events;
	uint64_t last_ack_time;
};

struct mux_device
{
	struct usb_device *usbdev;
	int id;
	enum mux_dev_state state;
	int visible;
	struct collection connections;
	uint16_t next_sport;
	unsigned char *pktbuf;
	uint32_t pktlen;
	void *preflight_cb_data;
	int version;
	uint16_t rx_seq;
	uint16_t tx_seq;

	#ifndef USE_PORTDRIVER_SOCKETS
		SOCKET rx_data_events_socket;
	#endif

	int is_preflight_worker_running;
};

static struct collection device_list;
pthread_mutex_t device_list_mutex;

static struct mux_device* get_mux_device_for_id(int device_id)
{
  struct mux_device *dev = NULL;
	pthread_mutex_lock(&device_list_mutex);
	FOREACH(struct mux_device *cdev, &device_list, struct mux_device *) {
		if(cdev->id == device_id) {
			dev = cdev;
			break;
		}
	} ENDFOREACH
	pthread_mutex_unlock(&device_list_mutex);

	return dev;
}

static struct mux_connection* get_mux_connection(int device_id, struct mux_client *client)
{
	struct mux_connection *conn = NULL;
	pthread_mutex_lock(&device_list_mutex);
	FOREACH(struct mux_device *dev, &device_list, struct mux_device *) {
		if(dev->id == device_id) {
			FOREACH(struct mux_connection *lconn, &dev->connections, struct mux_connection *) {
				if(lconn->client == client) {
					conn = lconn;
					break;
				}
			} ENDFOREACH
			break;
		}
	} ENDFOREACH
	pthread_mutex_unlock(&device_list_mutex);

	return conn;
}

static int get_next_device_id(void)
{
	while(1) {
		int ok = 1;
		pthread_mutex_lock(&device_list_mutex);
		FOREACH(struct mux_device *dev, &device_list, struct mux_device *) {
			if(dev->id == next_device_id) {
				next_device_id++;
				ok = 0;
				break;
			}
		} ENDFOREACH
		pthread_mutex_unlock(&device_list_mutex);
		if(ok)
			return next_device_id++;
	}
}

static int send_packet(struct mux_device *dev, enum mux_protocol proto, void *header, const void *data, int length)
{
	unsigned char *buffer;
	int hdrlen;
	int res;

	switch(proto) {
		case MUX_PROTO_VERSION:
			hdrlen = sizeof(struct version_header);
			break;
		case MUX_PROTO_SETUP:
			hdrlen = 0;
			break;
		case MUX_PROTO_TCP:
			hdrlen = sizeof(struct tcphdr);
			break;
		default:
			usbmuxd_log(LL_ERROR, "Invalid protocol %d for outgoing packet (dev %d hdr %p data %p len %d)", proto, dev->id, header, data, length);
			return -1;
	}
	usbmuxd_log(LL_SPEW, "send_packet(%d, 0x%x, %p, %p, %d)", dev->id, proto, header, data, length);

	int mux_header_size = ((dev->version < 2) ? 8 : sizeof(struct mux_header));

	int total = mux_header_size + hdrlen + length;

	if(total > USB_MTU) {
		usbmuxd_log(LL_ERROR, "Tried to send packet larger than USB MTU (hdr %d data %d total %d) to device %d", hdrlen, length, total, dev->id);
		return -1;
	}

	buffer = (unsigned char *)malloc(total);
	struct mux_header *mhdr = (struct mux_header *)buffer;
	mhdr->protocol = htonl(proto);
	mhdr->length = htonl(total);
	if (dev->version >= 2) {
		mhdr->magic = htonl(0xfeedface);
		if (proto == MUX_PROTO_SETUP) {
			dev->tx_seq = 0;
			dev->rx_seq = 0xFFFF;
		}
		mhdr->tx_seq = htons(dev->tx_seq);
		mhdr->rx_seq = htons(dev->rx_seq);
		dev->tx_seq++;
	}	
	memcpy(buffer + mux_header_size, header, hdrlen);
	if(data && length)
		memcpy(buffer + mux_header_size + hdrlen, data, length);

	if((res = usb_send(dev->usbdev, buffer, total)) < 0) {
		usbmuxd_log(LL_ERROR, "usb_send failed while sending packet (len %d) to device %d: %d", total, dev->id, res);
		free(buffer);
		return res;
	}
	return total;
}

static uint16_t find_sport(struct mux_device *dev)
{
	if(collection_count(&dev->connections) >= 65535)
		return 0; //insanity

	while(1) {
		int ok = 1;
		FOREACH(struct mux_connection *conn, &dev->connections, struct mux_connection *) {
			if(dev->next_sport == conn->sport) {
				dev->next_sport++;
				ok = 0;
				break;
			}
		} ENDFOREACH
		if(ok)
			return dev->next_sport++;
	}
}

static int send_anon_rst(struct mux_device *dev, uint16_t sport, uint16_t dport, uint32_t ack)
{
	struct tcphdr th;
	memset(&th, 0, sizeof(th));
	th.th_sport = htons(sport);
	th.th_dport = htons(dport);
	th.th_ack = htonl(ack);
	th.th_flags = TH_RST;
	th.th_off = sizeof(th) / 4;

	usbmuxd_log(LL_DEBUG, "[OUT] dev=%d sport=%d dport=%d flags=0x%x", dev->id, sport, dport, th.th_flags);

	int res = send_packet(dev, MUX_PROTO_TCP, &th, NULL, 0);
	return res;
}

static int send_tcp(struct mux_connection *conn, uint8_t flags, const unsigned char *data, int length)
{
	struct tcphdr th;
	memset(&th, 0, sizeof(th));
	th.th_sport = htons(conn->sport);
	th.th_dport = htons(conn->dport);
	th.th_seq = htonl(conn->tx_seq);
	th.th_ack = htonl(conn->tx_ack);
	th.th_flags = flags;
	th.th_off = sizeof(th) / 4;
	th.th_win = htons(conn->tx_win >> 8);

	usbmuxd_log(LL_DEBUG, "[OUT] dev=%d sport=%d dport=%d seq=%d ack=%d flags=0x%x window=%d[%d] len=%d",
		conn->dev->id, conn->sport, conn->dport, conn->tx_seq, conn->tx_ack, flags, conn->tx_win, conn->tx_win >> 8, length);

	int res = send_packet(conn->dev, MUX_PROTO_TCP, &th, data, length);
	if(res >= 0) {
		conn->tx_acked = conn->tx_ack;
		conn->last_ack_time = mstime64();
		conn->flags &= ~CONN_ACK_PENDING;
	}
	return res;
}

static void connection_teardown(struct mux_connection *conn)
{
	int res;
	int size;

	if(conn->state == CONN_DEAD)
		return;
	usbmuxd_log(LL_DEBUG, "connection_teardown dev %d sport %d dport %d", conn->dev->id, conn->sport, conn->dport);
	if(conn->dev->state != MUXDEV_DEAD && conn->state != CONN_DYING && conn->state != CONN_REFUSED) {
		res = send_tcp(conn, TH_RST, NULL, 0);
		if(res < 0)
			usbmuxd_log(LL_ERROR, "Error sending TCP RST to device %d (%d->%d)", conn->dev->id, conn->sport, conn->dport);
	}
	if(conn->client) {
		if(conn->state == CONN_REFUSED || conn->state == CONN_CONNECTING) {
			client_notify_connect(conn->client, RESULT_CONNREFUSED);
		} else {
			conn->state = CONN_DEAD;
			if((conn->events & POLLOUT) && conn->ib_size > 0){
				while(1){
					size = client_write(conn->client, conn->ib_buf, conn->ib_size);
					if(size <= 0) {
						break;
					}
					if(size == (int)conn->ib_size) {
						conn->ib_size = 0;
						break;
					} else {
						conn->ib_size -= size;
						memmove(conn->ib_buf, conn->ib_buf + size, conn->ib_size);
					}
				}
			}
			client_close(conn->client);
		}
	}
	if(conn->ib_buf)
		free(conn->ib_buf);
	if(conn->ob_buf)
		free(conn->ob_buf);
	collection_remove(&conn->dev->connections, conn);
	free(conn);
}

#ifndef USE_PORTDRIVER_SOCKETS
int device_accept_socket(int listenfd, int reject_connection)
{
	struct sockaddr_in addr;
	int new_sock_fd;
	int len = sizeof(struct sockaddr_in);
	new_sock_fd = accept(listenfd, (struct sockaddr *)&addr, &len);
	if (new_sock_fd < 0)
	{
		usbmuxd_log(LL_ERROR, "accept() failed (%s)", strerror(errno));
		return new_sock_fd;
	}

	if (reject_connection) {
		usbmuxd_log(LL_WARNING, "rejecting new device connection");
		closesocket(new_sock_fd);
		return 0;
	}

	/* Disable buffering of small pakcets */
	int tcp_no_delay = 1;
	if (0 != setsockopt(new_sock_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&tcp_no_delay, sizeof(tcp_no_delay)))
	{
		usbmuxd_log(LL_ERROR, "setsockopt() failed (%s)", strerror(errno));
		closesocket(new_sock_fd);
		return -1;
	}

	/* Get the usb device id */
	int usb_dev_id = 0;
	if (sizeof(usb_dev_id) != recv(new_sock_fd, (char *)&usb_dev_id, sizeof(usb_dev_id), 0))
	{
		usbmuxd_log(LL_ERROR, "recv() failed (%s)", strerror(errno));
		closesocket(new_sock_fd);
		return -1;
	}

	/* Get the usb device */
	usb_device * usb_dev = usb_get_device_by_id(usb_dev_id);
	if (NULL == usb_dev)
	{
		usbmuxd_log(LL_ERROR, "Invalid usb device id");
		closesocket(new_sock_fd);
		return -1;
	}

	/* Store the socket */
	struct mux_device * dev = NULL;
	pthread_mutex_lock(&device_list_mutex);
	FOREACH(struct mux_device *cdev, &device_list, struct mux_device *)
	{
		if (cdev->usbdev == usb_dev)
		{
			dev = cdev;
			break;
		}
	} ENDFOREACH
	pthread_mutex_unlock(&device_list_mutex);
	if (!dev)
	{
		usbmuxd_log(LL_WARNING, "Attempted to connect a socket to a nonexistent device");
		closesocket(new_sock_fd);
		return -RESULT_BADDEV;
	}

	dev->rx_data_events_socket = new_sock_fd;

	return 0;
}

int device_process_sockets(fd_set * read_fds, fd_set * write_fds)
{
	pthread_mutex_lock(&device_list_mutex);
	FOREACH(struct mux_device *dev, &device_list, struct mux_device *)
	{
		if (INVALID_SOCKET != dev->rx_data_events_socket)
		{
			if (FD_ISSET(dev->rx_data_events_socket, read_fds))
			{
				/* Read the data signal from the socket (one byte, its value 
				 * doens't matter */
				char result = 0;
				if (sizeof(result) != recv(dev->rx_data_events_socket, &result, sizeof(result), 0))
				{
					usbmuxd_log(LL_ERROR, "recv() failed (%s)", strerror(errno));
					continue;
				}

				unsigned char * device_data = NULL;
				uint32_t device_data_size = 0;

				if (usb_get_read_result(dev->usbdev, (void **)&device_data, &device_data_size) < 0)
				{
					usbmuxd_log(LL_ERROR, "usb_get_read_result has failed");
					continue;
				}

				uint32_t data_processed = 0;
				while (data_processed < device_data_size)
				{
					data_processed += device_data_input(dev->usbdev, device_data + data_processed, device_data_size - data_processed);
				}
				

				/* Tell the usb device thread we've processed the current data
				 * (currently it doesn't really matter what we send) */
				result = 1;
				if (sizeof(result) != send(dev->rx_data_events_socket, &result, sizeof(result), 0))
				{
					usbmuxd_log(LL_ERROR, "send() failed (%s)", strerror(errno));
				}
			}
		}
	} ENDFOREACH
	pthread_mutex_unlock(&device_list_mutex);

	return 0;
}

int device_add_fds(fd_set * read_fds, fd_set * write_fds)
{
	int fd_count = 0;
	pthread_mutex_lock(&device_list_mutex);
	FOREACH(struct mux_device *dev, &device_list, struct mux_device *)
	{
		if (INVALID_SOCKET != dev->rx_data_events_socket)
		{
			FD_SET(dev->rx_data_events_socket, read_fds);
			fd_count++;
		}
	} ENDFOREACH
	pthread_mutex_unlock(&device_list_mutex);

	return fd_count;
}
#endif /* USE_PORTDRIVER_SOCKETS */

int device_start_connect(int device_id, uint16_t dport, struct mux_client *client)
{
	struct mux_device *dev = get_mux_device_for_id(device_id);
	if(!dev) {
		usbmuxd_log(LL_WARNING, "Attempted to connect to nonexistent device %d", device_id);
		return -RESULT_BADDEV;
	}

	uint16_t sport = find_sport(dev);
	if(!sport) {
		usbmuxd_log(LL_WARNING, "Unable to allocate port for device %d", device_id);
		return -RESULT_BADDEV;
	}

	struct mux_connection *conn;
	conn = (struct mux_connection *)malloc(sizeof(struct mux_connection));
	memset(conn, 0, sizeof(struct mux_connection));

	conn->dev = dev;
	conn->client = client;
	conn->state = CONN_CONNECTING;
	conn->sport = sport;
	conn->dport = dport;
	conn->tx_seq = 0;
	conn->tx_ack = 0;
	conn->tx_acked = 0;
	conn->tx_win = CONN_INBUF_SIZE;
	conn->flags = 0;
	conn->max_payload = MAX_MUX_PACKET_SIZE - sizeof(struct mux_header) - sizeof(struct tcphdr);
	
	conn->ob_buf = (unsigned char *)malloc(CONN_OUTBUF_SIZE);
	conn->ob_capacity = CONN_OUTBUF_SIZE;
	conn->ib_buf = (unsigned char *)malloc(CONN_INBUF_SIZE);
	conn->ib_capacity = CONN_INBUF_SIZE;
	conn->ib_size = 0;

	int res;

	res = send_tcp(conn, TH_SYN, NULL, 0);
	if(res < 0) {
		usbmuxd_log(LL_ERROR, "Error sending TCP SYN to device %d (%d->%d)", dev->id, sport, dport);
		free(conn);
		return -RESULT_CONNREFUSED; //bleh
	}
	collection_add(&dev->connections, conn);
	return 0;
}

/**
 * Examine the state of a connection's buffers and
 * update all connection flags and masks accordingly.
 * Does not do I/O.
 *
 * @param conn The connection to update.
 */
static void update_connection(struct mux_connection *conn)
{
	uint32_t sent = conn->tx_seq - conn->rx_ack;

	if(conn->rx_win > sent)
		conn->sendable = conn->rx_win - sent;
	else
		conn->sendable = 0;

	if(conn->sendable > conn->ob_capacity)
		conn->sendable = conn->ob_capacity;
	if(conn->sendable > conn->max_payload)
		conn->sendable = conn->max_payload;

	if(conn->sendable > 0)
		conn->events |= POLLIN;
	else
		conn->events &= ~POLLIN;

	if(conn->ib_size)
		conn->events |= POLLOUT;
	else
		conn->events &= ~POLLOUT;

	if(conn->tx_acked != conn->tx_ack)
		conn->flags |= CONN_ACK_PENDING;
	else
		conn->flags &= ~CONN_ACK_PENDING;

	usbmuxd_log(LL_SPEW, "update_connection: sendable %d, events %d, flags %d", conn->sendable, conn->events, conn->flags);
	client_set_events(conn->client, conn->events);
}

static int send_tcp_ack(struct mux_connection *conn)
{
	if (send_tcp(conn, TH_ACK, NULL, 0) < 0) {
		usbmuxd_log(LL_ERROR, "Error sending TCP ACK (%d->%d)", conn->sport, conn->dport);
		connection_teardown(conn);
		return -1;
	}

	update_connection(conn);

	return 0;
}

/**
 * Flush input and output buffers for a client connection.
 *
 * @param device_id Numeric id for the device.
 * @param client The client to flush buffers for.
 * @param events event mask for the client. POLLOUT means that
 *   the client is ready to receive data, POLLIN that it has
 *   data to be read (and send along to the device).
 */
void device_client_process(int device_id, struct mux_client *client, short events)
{
	struct mux_connection *conn = get_mux_connection(device_id, client);

	if(!conn) {
		usbmuxd_log(LL_WARNING, "Could not find connection for device %d client %p", device_id, client);
		return;
	}
	usbmuxd_log(LL_SPEW, "device_client_process (%d)", events);

	int res;
	int size;
	if((events & POLLOUT) && (conn->ib_size > 0)) {
		// Client is ready to receive data, send what we have
		// in the client's connection buffer (if there is any)
		size = client_write(conn->client, conn->ib_buf, conn->ib_size);
		if(size <= 0) {
			usbmuxd_log(LL_DEBUG, "error writing to client (%d)", size);
			connection_teardown(conn);
			return;
		}

		if(size == (int)conn->ib_size) {
			conn->ib_size = 0;
		} else {
			conn->ib_size -= size;
			memmove(conn->ib_buf, conn->ib_buf + size, conn->ib_size);
		}

		// Update the connection's tx window. If the current windows is small
		// (smaller than the max packet size, for maximizing the usb transfers),
		// we'll let the device know we've updated our window.
		int should_advertise_new_window = conn->tx_win < MAX_MUX_PACKET_SIZE;
		conn->tx_win += size;

		if (should_advertise_new_window) {
			send_tcp_ack(conn);
		}
	}
	if((events & POLLIN) && (conn->sendable > 0)) {
		// There is inbound trafic on the client socket,
		// convert it to tcp and send to the device
		// (if the device's input buffer is not full)
		size = client_read(conn->client, conn->ob_buf, conn->sendable);
		if(size <= 0) {
			if (size < 0) {
				usbmuxd_log(LL_DEBUG, "error reading from client (%d)", size);
			}
			connection_teardown(conn);
			return;
		}
		res = send_tcp(conn, TH_ACK, conn->ob_buf, size);
		if(res < 0) {
			connection_teardown(conn);
			return;
		}
		conn->tx_seq += size;
	}

	update_connection(conn);
}

/**
 * Copy a payload to a connection's in-buffer and
 * set the POLLOUT event mask on the connection so
 * the next main_loop iteration will dispatch the
 * buffer if the connection socket is writable.
 *
 * Connection buffers are flushed in the
 * device_client_process() function.
 *
 * @param conn The connection to add incoming data to.
 * @param payload Payload to prepare for writing.
 *   The payload will be copied immediately so you are
 *   free to alter or free the payload buffer when this
 *   function returns.
 * @param payload_length number of bytes to copy from from
 *   the payload.
 */
static void connection_device_input(struct mux_connection *conn, unsigned char *payload, uint32_t payload_length)
{
	if((conn->ib_size + payload_length) > conn->ib_capacity) {
		usbmuxd_log(LL_ERROR, "Input buffer overflow on device %d connection %d->%d (space=%d, payload=%d)", conn->dev->id, conn->sport, conn->dport, conn->ib_capacity-conn->ib_size, payload_length);
		connection_teardown(conn);
		return;
	}
	memcpy(conn->ib_buf + conn->ib_size, payload, payload_length);
	conn->ib_size += payload_length;
	conn->tx_win -= payload_length;
	conn->tx_ack += payload_length;
	update_connection(conn);
}

void device_abort_connect(int device_id, struct mux_client *client)
{
  struct mux_connection *conn = get_mux_connection(device_id, client);
	if (conn) {
		connection_teardown(conn);
	} else {
		usbmuxd_log(LL_WARNING, "Attempted to abort for nonexistent connection for device %d", device_id);
	}
}

static void device_version_input(struct mux_device *dev, struct version_header *vh)
{
	if(dev->state != MUXDEV_INIT) {
		usbmuxd_log(LL_WARNING, "Version packet from already initialized device %d", dev->id);
		return;
	}
	vh->major = ntohl(vh->major);
	vh->minor = ntohl(vh->minor);
	if(vh->major != 2 && vh->major != 1) {
		usbmuxd_log(LL_ERROR, "Device %d has unknown version %d.%d", dev->id, vh->major, vh->minor);
		pthread_mutex_lock(&device_list_mutex);
		collection_remove(&device_list, dev);
		pthread_mutex_unlock(&device_list_mutex);
		free(dev);
		return;
	}
	dev->version = vh->major;

	if (dev->version >= 2) {
		send_packet(dev, MUX_PROTO_SETUP, NULL, "\x07", 1);
	}

	usbmuxd_log(LL_NOTICE, "Connected to v%d.%d device %d on location 0x%x with serial number %s", dev->version, vh->minor, dev->id, usb_get_location(dev->usbdev), usb_get_serial(dev->usbdev));
	dev->state = MUXDEV_ACTIVE;
	collection_init(&dev->connections);
	struct device_info info;
	info.id = dev->id;
	info.location = usb_get_location(dev->usbdev);
	info.serial = usb_get_serial(dev->usbdev);
	info.pid = usb_get_pid(dev->usbdev);
	preflight_worker_device_add(&info);
	dev->is_preflight_worker_running = 1;
}

static void device_control_input(struct mux_device *dev, unsigned char *payload, uint32_t payload_length)
{
	if (payload_length > 0) {
		switch (payload[0]) {
		case 3:
			if (payload_length > 1) {
				char* buf = (char* )malloc(payload_length);
				strncpy(buf, (char*)payload+1, payload_length-1);
				buf[payload_length-1] = '\0';
				usbmuxd_log(LL_ERROR, "%s: ERROR: %s", __func__, buf);
				free(buf);
			} else {
				usbmuxd_log(LL_ERROR, "%s: Error occured, but empty error message", __func__);
			}
			break;
		case 7:
			if (payload_length > 1) {
				char* buf = (char*)malloc(payload_length);
				strncpy(buf, (char*)payload+1, payload_length-1);
				buf[payload_length-1] = '\0';
				usbmuxd_log(LL_INFO, "%s: %s", __func__, buf);
				free(buf);
			}
			break;
		default:
			break;
		}
	} else {
		usbmuxd_log(LL_WARNING, "%s: got a type 1 packet without payload", __func__);
	}
}

/**
 * Handle an incoming TCP packet from the device.
 *
 * @param dev The device handle TCP input on.
 * @param th Pointer to the TCP header struct.
 * @param payload Payload data.
 * @param payload_length Number of bytes in payload.
 */
static void device_tcp_input(struct mux_device *dev, struct tcphdr *th, unsigned char *payload, uint32_t payload_length)
{
	uint16_t sport = ntohs(th->th_dport);
	uint16_t dport = ntohs(th->th_sport);
	struct mux_connection *conn = NULL;

	usbmuxd_log(LL_DEBUG, "[IN] dev=%d sport=%d dport=%d seq=%d ack=%d flags=0x%x window=%d[%d] len=%d",
		dev->id, dport, sport, ntohl(th->th_seq), ntohl(th->th_ack), th->th_flags, ntohs(th->th_win) << 8, ntohs(th->th_win), payload_length);

	if(dev->state != MUXDEV_ACTIVE) {
		usbmuxd_log(LL_ERROR, "Received TCP packet from device %d but the device isn't active yet, discarding", dev->id);
		return;
	}

	// Find the connection on this device that has the right sport and dport
	FOREACH(struct mux_connection *lconn, &dev->connections, struct mux_connection *) {
		if(lconn->sport == sport && lconn->dport == dport) {
			conn = lconn;
			break;
		}
	} ENDFOREACH

	if(!conn) {
		if(!(th->th_flags & TH_RST)) {
			usbmuxd_log(LL_INFO, "No connection for device %d incoming packet %d->%d", dev->id, dport, sport);
			if(send_anon_rst(dev, sport, dport, ntohl(th->th_seq)) < 0)
				usbmuxd_log(LL_ERROR, "Error sending TCP RST to device %d (%d->%d)", conn->dev->id, sport, dport);
		}
		return;
	}

	conn->rx_seq = ntohl(th->th_seq);
	conn->rx_ack = ntohl(th->th_ack);
	conn->rx_win = ntohs(th->th_win) << 8;

	if(th->th_flags & TH_RST) {
		char *buf = (char *)malloc(payload_length+1);
		memcpy(buf, payload, payload_length);
		if(payload_length && (buf[payload_length-1] == '\n'))
			buf[payload_length-1] = 0;
		buf[payload_length] = 0;
		usbmuxd_log(LL_DEBUG, "RST reason: %s", buf);
		free(buf);
	}

	if(conn->state == CONN_CONNECTING) {
		if(th->th_flags != (TH_SYN|TH_ACK)) {
			if(th->th_flags & TH_RST)
				conn->state = CONN_REFUSED;
			usbmuxd_log(LL_INFO, "Connection refused by device %d (%d->%d)", dev->id, sport, dport);
			connection_teardown(conn); //this also sends the notification to the client
		} else {
			conn->tx_seq++;
			conn->tx_ack++;
			if(send_tcp(conn, TH_ACK, NULL, 0) < 0) {
				usbmuxd_log(LL_ERROR, "Error sending TCP ACK to device %d (%d->%d)", dev->id, sport, dport);
				connection_teardown(conn);
				return;
			}
			conn->state = CONN_CONNECTED;
			if(client_notify_connect(conn->client, RESULT_OK) < 0) {
				conn->client = NULL;
				connection_teardown(conn);
			}
			update_connection(conn);
		}
	} else if(conn->state == CONN_CONNECTED) {
		if(th->th_flags != TH_ACK) {
			usbmuxd_log(LL_INFO, "Connection reset by device %d (%d->%d)", dev->id, sport, dport);
			if(th->th_flags & TH_RST)
				conn->state = CONN_DYING;
			connection_teardown(conn);
		} else {
			connection_device_input(conn, payload, payload_length);
			if(conn->flags & CONN_ACK_PENDING)
				send_tcp_ack(conn);
		}
	}
}

/**
 * Take input data from the device that has been read into a buffer
 * and dispatch it to the right protocol backend (eg. TCP).
 *
 * @param usbdev
 * @param buffer
 * @param length
 */
uint32_t device_data_input(struct usb_device *usbdev, unsigned char *buffer, uint32_t length)
{
	struct mux_device *dev = NULL;
	pthread_mutex_lock(&device_list_mutex);
	FOREACH(struct mux_device *tdev, &device_list, struct mux_device *) {
		if(tdev->usbdev == usbdev) {
			dev = tdev;
			break;
		}
	} ENDFOREACH
	pthread_mutex_unlock(&device_list_mutex);
	if(!dev) {
		usbmuxd_log(LL_WARNING, "Cannot find device entry for RX input from USB device %p on location 0x%x", usbdev, usb_get_location(usbdev));
		return length;
	}

	if(!length)
		return length;

	// sanity check (should never happen with current USB implementation)
	if(/*(length > USB_MRU) ||*/ (length > DEV_MRU)) {
		usbmuxd_log(LL_ERROR, "Too much data received from USB (%d), file a bug", length);
		return length;
	}

	usbmuxd_log(LL_SPEW, "Mux data input for device %p: %p len %d", dev, buffer, length);

	// handle broken up transfers
	if(dev->pktlen) {
		if((length + dev->pktlen) > DEV_MRU) {
			usbmuxd_log(LL_ERROR, "Incoming split packet is too large (%d so far), dropping!", length + dev->pktlen);
			dev->pktlen = 0;
			return length;
		}
		memcpy(dev->pktbuf + dev->pktlen, buffer, length);
		struct mux_header *mhdr = (struct mux_header *)dev->pktbuf;
		if((length < USB_MRU) || (ntohl(mhdr->length) == (length + dev->pktlen))) {
			buffer = dev->pktbuf;
			length += dev->pktlen;
			dev->pktlen = 0;
			usbmuxd_log(LL_SPEW, "Gathered mux data from buffer (total size: %d)", length);
		} else {
			dev->pktlen += length;
			usbmuxd_log(LL_SPEW, "Appended mux data to buffer (total size: %d)", dev->pktlen);
			return length;
		}
	} else {
		struct mux_header *mhdr = (struct mux_header *)buffer;
		if((length == USB_MRU) && (length < ntohl(mhdr->length))) {
			memcpy(dev->pktbuf, buffer, length);
			dev->pktlen = length;
			usbmuxd_log(LL_SPEW, "Copied mux data to buffer (size: %d)", dev->pktlen);
			return length;
		}
	}

	struct mux_header *mhdr = (struct mux_header *)buffer;
	int mux_header_size = ((dev->version < 2) ? 8 : sizeof(struct mux_header));
	if(ntohl(mhdr->length) > length) {
		usbmuxd_log(LL_ERROR, "Incoming packet size mismatch (dev %d, expected %d, got %d)", dev->id, ntohl(mhdr->length), length);
		return length;
	}

	struct tcphdr *th;
	unsigned char *payload;
	uint32_t payload_length;

	if (dev->version >= 2) {
		dev->rx_seq = ntohs(mhdr->rx_seq);
	}

	switch(ntohl(mhdr->protocol)) {
		case MUX_PROTO_VERSION:
			if(length < (mux_header_size + sizeof(struct version_header))) {
				usbmuxd_log(LL_ERROR, "Incoming version packet is too small (%d)", length);
				return length;
			}
			device_version_input(dev, (struct version_header *)((char*)mhdr+mux_header_size));
			break;
		case MUX_PROTO_CONTROL:
			payload = (unsigned char *)(mhdr+1);
			payload_length = length - mux_header_size;
			device_control_input(dev, payload, payload_length);
			break;
		case MUX_PROTO_TCP:
			if(length < (mux_header_size + sizeof(struct tcphdr))) {
				usbmuxd_log(LL_ERROR, "Incoming TCP packet is too small (%d)", length);
				return length;
			}
			th = (struct tcphdr *)((char*)mhdr+mux_header_size);
			payload = (unsigned char *)(th+1);
			payload_length = length - sizeof(struct tcphdr) - mux_header_size;
			device_tcp_input(dev, th, payload, payload_length);
			break;
		default:
			usbmuxd_log(LL_ERROR, "Incoming packet for device %d has unknown protocol 0x%x)", dev->id, ntohl(mhdr->protocol));
			return length;
	}

	if (ntohl(mhdr->length) < length)
	{
		usbmuxd_log(LL_INFO, "Incoming buffer contains more than one packet");
		return ntohl(mhdr->length);
	}

	return length;
}

int device_add(struct usb_device *usbdev)
{
	int res;
	int id = get_next_device_id();
	struct mux_device *dev;
	usbmuxd_log(LL_NOTICE, "Connecting to new device on location 0x%x as ID %d", usb_get_location(usbdev), id);
	dev = (struct mux_device *)malloc(sizeof(struct mux_device));
	dev->id = id;
	dev->usbdev = usbdev;
	dev->state = MUXDEV_INIT;
	dev->visible = 0;
	dev->next_sport = 1;
	dev->pktbuf = (unsigned char *)malloc(DEV_MRU);
	dev->pktlen = 0;
	dev->preflight_cb_data = NULL;
	dev->is_preflight_worker_running = 0;
	dev->version = 0;
	#ifndef USE_PORTDRIVER_SOCKETS
		dev->rx_data_events_socket = INVALID_SOCKET;
	#endif

	struct version_header vh;
	vh.major = htonl(1);
	vh.minor = htonl(0);
	vh.padding = 0;
	if((res = send_packet(dev, MUX_PROTO_VERSION, &vh, NULL, 0)) < 0) {
		usbmuxd_log(LL_ERROR, "Error sending version request packet to device %d", id);
		free(dev);
		return res;
	}
	pthread_mutex_lock(&device_list_mutex);
	collection_add(&device_list, dev);
	pthread_mutex_unlock(&device_list_mutex);
	return 0;
}

void device_remove(struct usb_device *usbdev)
{
	pthread_mutex_lock(&device_list_mutex);
	FOREACH(struct mux_device *dev, &device_list, struct mux_device *) {
		if(dev->usbdev == usbdev) {
			usbmuxd_log(LL_NOTICE, "Removed device %d on location 0x%x", dev->id, usb_get_location(usbdev));
			if(dev->state == MUXDEV_ACTIVE) {
				dev->state = MUXDEV_DEAD;
				FOREACH(struct mux_connection *conn, &dev->connections, struct mux_connection *) {
					connection_teardown(conn);
				} ENDFOREACH
				
				if (dev->visible) {
					client_device_remove(dev->id);
				} else {
					usbmuxd_log(LL_NOTICE, "Removed device %d on location 0x%x was removed while invisible", dev->id, usb_get_location(usbdev));
					device_info dev_info = { 0 };
					dev_info.id = dev->id;
					dev_info.serial = usb_get_serial(dev->usbdev);
					dev_info.location = usb_get_location(dev->usbdev);
					dev_info.pid = usb_get_pid(dev->usbdev);
					client_device_removed_during_add(&dev_info);
				}
				
				collection_free(&dev->connections);
			}
			if (dev->preflight_cb_data) {
				preflight_device_remove_cb(dev->preflight_cb_data);
			}
			collection_remove(&device_list, dev);
			pthread_mutex_unlock(&device_list_mutex);
			free(dev->pktbuf);
			#ifndef USE_PORTDRIVER_SOCKETS
				closesocket(dev->rx_data_events_socket);
			#endif
			free(dev);
			return;
		}
	} ENDFOREACH
	pthread_mutex_unlock(&device_list_mutex);

	usbmuxd_log(LL_WARNING, "Cannot find device entry while removing USB device %p on location 0x%x", usbdev, usb_get_location(usbdev));
}

void device_add_failed(struct usb_device *dev)
{
	usbmuxd_log(LL_NOTICE, "Notifying clients we've failed to add device at location 0x%x", usb_get_location(dev));
	device_info dev_info = { 0 };
	dev_info.id = 0;
	dev_info.serial = usb_get_serial(dev);
	dev_info.location = usb_get_location(dev);
	dev_info.pid = usb_get_pid(dev);
	client_device_removed_during_add(&dev_info);
}

void device_set_visible(int device_id)
{
	pthread_mutex_lock(&device_list_mutex);
	FOREACH(struct mux_device *dev, &device_list, struct mux_device *) {
		if(dev->id == device_id) {
			dev->visible = 1;
			usb_set_device_ready(dev->usbdev);
			break;
		}
	} ENDFOREACH
	pthread_mutex_unlock(&device_list_mutex);
}

void device_set_preflight_cb_data(int device_id, void* data)
{
	pthread_mutex_lock(&device_list_mutex);
	FOREACH(struct mux_device *dev, &device_list, struct mux_device *) {
		if(dev->id == device_id) {
			dev->preflight_cb_data = data;
			break;
		}
	} ENDFOREACH
	pthread_mutex_unlock(&device_list_mutex);
}

int device_get_count(int include_hidden)
{
	int count = 0;
	struct collection dev_list = {NULL, 0};
	pthread_mutex_lock(&device_list_mutex);
	collection_copy(&dev_list, &device_list);
	pthread_mutex_unlock(&device_list_mutex);

	FOREACH(struct mux_device *dev, &dev_list, struct mux_device *) {
		if((dev->state == MUXDEV_ACTIVE) && (include_hidden || dev->visible))
			count++;
	} ENDFOREACH

	collection_free(&dev_list);
	return count;
}

int device_get_list(int include_hidden, struct device_info **devices)
{
	int count = 0;
	struct collection dev_list = {NULL, 0};
	pthread_mutex_lock(&device_list_mutex);
	collection_copy(&dev_list, &device_list);
	pthread_mutex_unlock(&device_list_mutex);

	*devices = (struct device_info *)malloc(sizeof(struct device_info) * dev_list.capacity);
	struct device_info *p = *devices;

	FOREACH(struct mux_device *dev, &device_list, struct mux_device *) {
		if((dev->state == MUXDEV_ACTIVE) && (include_hidden || dev->visible)) {
			p->id = dev->id;
			p->serial = usb_get_serial(dev->usbdev);
			p->location = usb_get_location(dev->usbdev);
			p->pid = usb_get_pid(dev->usbdev);
			count++;
			p++;
		}
	} ENDFOREACH

	collection_free(&dev_list);

	return count;
}

int device_get_timeout(void)
{
	uint64_t oldest = (uint64_t)-1LL;
	pthread_mutex_lock(&device_list_mutex);
	FOREACH(struct mux_device *dev, &device_list, struct mux_device *) {
		if(dev->state == MUXDEV_ACTIVE) {
			FOREACH(struct mux_connection *conn, &dev->connections, struct mux_connection *) {
				if((conn->state == CONN_CONNECTED) && (conn->flags & CONN_ACK_PENDING) && conn->last_ack_time < oldest)
					oldest = conn->last_ack_time;
			} ENDFOREACH
		}
	} ENDFOREACH
	pthread_mutex_unlock(&device_list_mutex);
	uint64_t ct = mstime64();
	if((int64_t)oldest == -1LL)
		return 100000; //meh
	if((ct - oldest) > ACK_TIMEOUT)
		return 0;
	return ACK_TIMEOUT - (ct - oldest);
}

void device_check_timeouts(void)
{
	uint64_t ct = mstime64();
	pthread_mutex_lock(&device_list_mutex);
	FOREACH(struct mux_device *dev, &device_list, struct mux_device *) {
		if(dev->state == MUXDEV_ACTIVE) {
			FOREACH(struct mux_connection *conn, &dev->connections, struct mux_connection *) {
				if((conn->state == CONN_CONNECTED) && 
						(conn->flags & CONN_ACK_PENDING) && 
						(ct - conn->last_ack_time) > ACK_TIMEOUT) {
					usbmuxd_log(LL_DEBUG, "Sending ACK due to expired timeout (%" PRIu64 " -> %" PRIu64 ")", conn->last_ack_time, ct);
					send_tcp_ack(conn);
				}
			} ENDFOREACH
		}
	} ENDFOREACH
	pthread_mutex_unlock(&device_list_mutex);
}

void device_init(void)
{
	usbmuxd_log(LL_DEBUG, "device_init");
	collection_init(&device_list);
	pthread_mutex_init(&device_list_mutex, NULL);
	next_device_id = 1;
}

void device_kill_connections(void)
{
	usbmuxd_log(LL_DEBUG, "device_kill_connections");
	FOREACH(struct mux_device *dev, &device_list, struct mux_device *) {
		if(dev->state != MUXDEV_INIT) {
			FOREACH(struct mux_connection *conn, &dev->connections, struct mux_connection *) {
				connection_teardown(conn);
			} ENDFOREACH
		}
	} ENDFOREACH
	// give USB a while to send the final connection RSTs and the like
	Sleep(100);
}

void device_shutdown(void)
{
	usbmuxd_log(LL_DEBUG, "device_shutdown");
	pthread_mutex_lock(&device_list_mutex);
	FOREACH(struct mux_device *dev, &device_list, struct mux_device *) {
		FOREACH(struct mux_connection *conn, &dev->connections, struct mux_connection *) {
			connection_teardown(conn);
		} ENDFOREACH
		collection_free(&dev->connections);
		collection_remove(&device_list, dev);
		free(dev);
	} ENDFOREACH
	pthread_mutex_unlock(&device_list_mutex);
	pthread_mutex_destroy(&device_list_mutex);
	collection_free(&device_list);
}

void device_lock_devices()
{
	pthread_mutex_lock(&device_list_mutex);
}

void device_unlock_devices()
{
	pthread_mutex_unlock(&device_list_mutex);
}

int device_exists(int device_id, int lock_devices)
{
	if (lock_devices) {
		pthread_mutex_lock(&device_list_mutex);
	}
	
	int found = 0;
	FOREACH(struct mux_device *dev, &device_list, struct mux_device *) {
		if(dev->id == device_id) {
			found = 1;
			break;
		}
	} ENDFOREACH

	if (lock_devices) {
		pthread_mutex_unlock(&device_list_mutex);
	}

	return found;
}

int device_is_initializing(struct usb_device *usb_dev)
{
	struct mux_device * mux_dev = NULL;

	pthread_mutex_lock(&device_list_mutex);
	FOREACH(struct mux_device *cur_mux_dev, &device_list, struct mux_device *) {
		if(usb_get_location(usb_dev) == usb_get_location(cur_mux_dev->usbdev)) {
			mux_dev = cur_mux_dev;
			break;
		}
	} ENDFOREACH
	pthread_mutex_unlock(&device_list_mutex);

	if (NULL == mux_dev) {
		return 0;
	}

	if ((MUXDEV_INIT == mux_dev->state) || (mux_dev->is_preflight_worker_running)) {
		return 1;
	}

	return 0;
}

void device_preflight_finished(int device_id)
{
	pthread_mutex_lock(&device_list_mutex);
	FOREACH(struct mux_device *dev, &device_list, struct mux_device *) {
		if(dev->id == device_id) {
			dev->is_preflight_worker_running = 0;
			break;
		}
	} ENDFOREACH
	pthread_mutex_unlock(&device_list_mutex);
}