/******************************************************************************
 * usb-mce_internal.h
 *****************************************************************************/
#ifndef __USBMUXD_USB_MCE_INTERNAL_H__
#define __USBMUXD_USB_MCE_INTERNAL_H__

/******************************************************************************
 * Defs & Types
 *****************************************************************************/
#define IS_READ_ENDPOINT(ep) (((ep) & 0x80) != 0)

#define READ_WAIT_EVENTS_ARR_STOP_EVENT (0)
#define READ_WAIT_EVENTS_ARR_DEVICE_EVENT (1)
#define READ_WAIT_TIMEOUT (5000)

#define READ_THREAD_SHUTDOWN_TIMEOUT (3000)

#define DEVICE_RX_BUFFER_SIZE (0x8008)

#ifdef USE_PORTDRIVER_SOCKETS
	#define NUM_RX_LOOPS (3)
#else
#include "readerwriterqueue\readerwriterqueue.h"
#include "usbmuxd_com_plugin_api.h"
	struct usb_device_rx
	{
		void		* buffer;
		uint32_t	data_size;
		OVERLAPPED	overlapped;
		HANDLE		thread_stop_event;
		HANDLE		thread;
		SOCKET		data_events_socket;
		usb_device_rx():
			buffer(0),
			data_size(0),
			thread_stop_event(0),
			thread(0),
			data_events_socket(0)
		{
			memset(&overlapped,0,sizeof(OVERLAPPED)); 
		}
	};
	struct usb_device_tx_q_element
	{
		void* buffer;
		OVERLAPPED*  theOverLapped;
	};
	struct usb_device_tx
	{
		HANDLE writeThreadStop;
		HANDLE		thread;
		moodycamel::BlockingReaderWriterQueue<usb_device_tx_q_element> q;
		moodycamel::BlockingReaderWriterQueue<usb_device_tx_q_element> pool;
		usb_device_tx() :
			writeThreadStop(0),
			thread(0)
		{

		}
	};

#endif

struct usb_device_instance_info
{
	char		serial[256];
	uint8_t		ep_in, ep_out;
	uint16_t	tx_max_packet_size;
	usb_device_instance_info() :
		ep_in(0),
		ep_out(0),
		tx_max_packet_size(0)
		
	{
		memset(&serial, 0, sizeof(serial));
	}
};

enum usb_device_state
{
	USB_DEVICE_STATE_ALIVE,
	USB_DEVICE_STATE_DEAD,
	USB_DEVICE_STATE_WAITING_FOR_DEVICE
};


struct usbmuxd_plugin{


};


struct usb_device
{
	
	int id;
	uint32_t location;
	COMHANDLE port;
	enum device_monitor_state monitor;
	enum usb_device_state state;
	
	struct usb_device_instance_info info;

	#ifdef USE_PORTDRIVER_SOCKETS
		struct collection rx_transfers;
	#else
		struct usb_device_rx rx;
		struct usb_device_tx tx;

	#endif

	HANDLE device_ready_event;



	usb_device() :
				 	id(0),
				 	location(0),
				 	monitor(device_monitor_state::DEVICE_MONITOR_ONCE),
				 	state(usb_device_state::USB_DEVICE_STATE_ALIVE),
					device_ready_event(0)
	{
		memset(&port, 0, sizeof(COMHANDLE));
	}
};

#define IS_VALID_DEVICE(dev) ((NULL != (dev)) && (COM_MCEUSB == ((dev)->port).dwPortID))

#define MCE_PORT_NAME_FORMAT ("MCE%u")

typedef enum _PENDING_DEVICE_COMMAND_TYPE
{
	PENDING_DEVICE_COMMAND_ADD,
	PENDING_DEVICE_COMMAND_REMOVE,
	PENDING_DEVICE_COMMAND_SET_MONITOR
} PENDING_DEVICE_COMMAND_TYPE;

typedef enum _PENDING_DEVICE_COMMAND_SOURCE
{
	PENDING_DEVICE_COMMAND_SOURCE_MANUAL,
	PENDING_DEVICE_COMMAND_SOURCE_MONITOR
} PENDING_DEVICE_COMMAND_SOURCE;

typedef struct _PENDING_DEVICE_COMMAND
{
	PENDING_DEVICE_COMMAND_TYPE type;
	PENDING_DEVICE_COMMAND_SOURCE source;
	uint32_t device_location;
	HANDLE completion_event;

	enum device_monitor_state monitor;
} PENDING_DEVICE_COMMAND;

#define LOCK_PENDING_DEVICES() EnterCriticalSection(&g_pending_devices_lock);
#define UNLOCK_PENDING_DEVICES() LeaveCriticalSection(&g_pending_devices_lock);

/******************************************************************************
 * Globals
 *****************************************************************************/
static struct collection g_device_list;
static int g_next_usb_device_id;
static CAtlList<PENDING_DEVICE_COMMAND *> g_pending_devices;
static CRITICAL_SECTION g_pending_devices_lock;
static HANDLE g_port_notification_callback_cookie;

/******************************************************************************
 * Internal Functions Declarations
 *****************************************************************************/
static void usb_port_change_callback(void * context, const DWORD event, const char * port_name);
static int usb_append_device_command(PENDING_DEVICE_COMMAND_TYPE	type, 
									  PENDING_DEVICE_COMMAND_SOURCE source,
									  uint32_t						location,
									  void							* completion_event,
									  device_monitor_state			monitor_device);
static int usb_add_pending_device(PENDING_DEVICE_COMMAND_SOURCE	source, 
								  uint32_t						location, 
								  device_monitor_state			monitor,
								  HANDLE						device_ready_event);
static int usb_remove_pending_device(PENDING_DEVICE_COMMAND_SOURCE	source, 
									 uint32_t						location,
									 HANDLE							remove_completed_event);
static int usb_handle_device_monitor_command(uint32_t				device_location,
											 device_monitor_state	monitor,
											 HANDLE					completion_event);
static void usb_update_monitored_devices();
static void usb_disconnect(struct usb_device * dev, bool manual_remove);
static bool usb_configure_device(struct usb_device * usb_dev);
static bool usb_configure_mux_interface(struct usb_device * usb_dev, unsigned char * config_desc);
static void usb_handle_port_failure(struct usb_device * dev,const char* caller, int le);

static void usb_free_device(struct usb_device *dev);

static void usb_report_device_already_exists(struct usb_device * dev);

#ifdef USE_PORTDRIVER_SOCKETS
	static void rx_callback(PORT_TRANSFER * ptTransfer);
	static int start_rx_loop(struct usb_device *dev);
	static int start_reading_from_device(struct usb_device * dev);
#else
	static DWORD WINAPI usb_read_thread_proc(void * context);
	static int usb_start_read_thread(usb_device * dev);
#endif

#endif /* __USBMUXD_USB_MCE_INTERNAL_H__ */