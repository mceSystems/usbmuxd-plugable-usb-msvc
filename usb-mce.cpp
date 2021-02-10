/******************************************************************************
 * usb-mce.cpp
 *****************************************************************************/
/******************************************************************************
 * Includes
 *****************************************************************************/
#include "stdafx.h"
#include "usb.h"
#include "device.h"
#include "client.h"
#include "utils.h"
#include "usbmuxd.h"
#include "usb-mce_internal.h"
#include "WindowsUtil.h"
/* MCE Includes */

#include "usbmuxd_com_plugin_client.h"

#include "SocketUtil.h"

#include <Dbt.h>
#include "usb100.h"



/******************************************************************************
 * usb_init Function
 *****************************************************************************/
int usb_init(uint32_t hub_address, LPCWSTR pLuginPath)
{
	/* Load the the port driver */
	if (com_plugin_client_init(pLuginPath) != COM_OK || com_plugin_init(hub_address) != COM_OK)
	{
		return -1; 
	}

	

	g_port_notification_callback_cookie = NULL;
	g_next_usb_device_id = 1;
	collection_init(&g_device_list);
	InitializeCriticalSection(&g_pending_devices_lock);

	return 0;
}

/******************************************************************************
 * usb_shutdown Function
 *****************************************************************************/
void usb_shutdown(void)
{
	/* Remove each connected device */
	FOREACH(struct usb_device *usbdev, &g_device_list, struct usb_device *)
	{
		device_remove(usbdev);
		usb_disconnect(usbdev, true);
	} ENDFOREACH

	/* Release any pending device commands */
	LOCK_PENDING_DEVICES();
	if (false == g_pending_devices.IsEmpty())
	{
		size_t pending_commands_count = g_pending_devices.GetCount();
		for (size_t i = 0; i < pending_commands_count; i++)
		{
			HEAP_FREE(g_pending_devices.RemoveHead());
		}
	}
	UNLOCK_PENDING_DEVICES();
	
	/* Unregister the port change notification */
	if (g_port_notification_callback_cookie)
	{
		if (COM_OK != com_plugin_unregister_com_notification(g_port_notification_callback_cookie))
		{
			DEBUG_PRINT_WIN32_ERROR("PortUnRegisterPortNotification");
		}
		g_port_notification_callback_cookie = NULL;
	}

	DeleteCriticalSection(&g_pending_devices_lock);
	collection_free(&g_device_list);

	com_plugin_deinit();
}

/******************************************************************************
 * usb_get_device_by_id Function
 *****************************************************************************/
usb_device * usb_get_device_by_id(int id)
{
	FOREACH(struct usb_device * dev, &g_device_list, struct usb_device *)
	{
		if (IS_VALID_DEVICE(dev) && (dev->id == id))
		{
			return dev;
		}
	} ENDFOREACH

	return NULL;
}

/******************************************************************************
 * usb_add_device Function
 *****************************************************************************/
int usb_add_device(uint32_t device_location, void * completion_event)
{
	usb_append_device_command(PENDING_DEVICE_COMMAND_ADD, 
							  PENDING_DEVICE_COMMAND_SOURCE_MANUAL,
							  device_location,
							  completion_event, 
							  DEVICE_MONITOR_DISABLE);
	return 0;
}

/******************************************************************************
 * usb_remove_device Function
 *****************************************************************************/
int usb_remove_device(uint32_t device_location, void * completion_event)
{
	usb_append_device_command(PENDING_DEVICE_COMMAND_REMOVE, 
							  PENDING_DEVICE_COMMAND_SOURCE_MANUAL,
							  device_location,
							  completion_event, 
							  DEVICE_MONITOR_DISABLE);
	return 0;
}

/******************************************************************************
 * usb_set_device_monitoring Function
 *****************************************************************************/
int usb_set_device_monitoring(uint32_t device_location, enum device_monitor_state monitor_device, uint32_t timeout)
{
	int ret = -1;

	/* Create a completion event */
	HANDLE completion_event = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (FALSE == IS_VALID_HANDLE(completion_event))
	{
		DEBUG_PRINT_WIN32_ERROR("CreateEvent");
		return -1;
	}

	usb_append_device_command(PENDING_DEVICE_COMMAND_SET_MONITOR, 
							  PENDING_DEVICE_COMMAND_SOURCE_MANUAL,
							  device_location,
							  completion_event,
							  monitor_device);

	/* Wait for the new setting to take effect */
	DWORD wait_result = WaitForSingleObject(completion_event, timeout);
	if (WAIT_OBJECT_0 == wait_result)
	{
		ret = 0;
	}
	else
	{
		if (WAIT_TIMEOUT == wait_result)
		{
			DEBUG_PRINT_ERROR("Timeout while waiting for device %d monitoring change", device_location);
		}
		else
		{
			DEBUG_PRINT_WIN32_ERROR("WaitForSingleObject");
		}
	}

	SAFE_CLOSE_HANDLE(completion_event);
	return ret;
}

/******************************************************************************
 * usb_set_device_monitoring_immediately Function
 *****************************************************************************/
int usb_set_device_monitoring_immediately(uint32_t device_location, enum device_monitor_state monitor_device)
{
	usb_append_device_command(PENDING_DEVICE_COMMAND_SET_MONITOR, 
							  PENDING_DEVICE_COMMAND_SOURCE_MANUAL,
							  device_location,
							  NULL,
							  monitor_device);
	
	/* Force the change to take effect immediately */
	return usb_process(NULL);
}

#ifdef USE_PORTDRIVER_SOCKETS
	/******************************************************************************
	 * usb_process Function
	 *****************************************************************************/
	int usb_process(fd_set * read_fds)
	{
		FOREACH(struct usb_device *usb_dev, &g_device_list, struct usb_device *)
		{
			/* Handle pending transfers */
			if (USB_DEVICE_STATE_ALIVE == usb_dev->state)
			{
				if (read_fds)
				{
					SOCKET endpoint_socket = INVALID_SOCKET;
					PortGetCompletionSocketForEndpoint(&(usb_dev->port), usb_dev->info.ep_in, &endpoint_socket);

					if (PORTOK != PortProcessPendingTransfers(&(usb_dev->port)))
					{
						LOG_WIN32_ERROR("PortProcessPendingTransfers");
					}
				}
			}
			/* If the device was marked as dead - remove it */
			else if (USB_DEVICE_STATE_DEAD == usb_dev->state)
			{
				LOG_TRACE("Removing a dead device");
				device_remove(usb_dev);
				usb_disconnect(usb_dev, PENDING_DEVICE_COMMAND_SOURCE_MONITOR);
			}
		} ENDFOREACH
	
		/* Handle pending devices */
		LOCK_PENDING_DEVICES();
		if (false == g_pending_devices.IsEmpty())
		{
			size_t pending_commands_count = g_pending_devices.GetCount();
			for (size_t i = 0; i < pending_commands_count; i++)
			{
				PENDING_DEVICE_COMMAND * pending_command = g_pending_devices.RemoveHead();
				switch (pending_command->type)
				{
				/* Add */
				case PENDING_DEVICE_COMMAND_ADD:
					usb_add_pending_device(pending_command->source, 
										   pending_command->device_location, 
										   pending_command->monitor,
										   pending_command->completion_event);
					break;

				/* Remove */
				case PENDING_DEVICE_COMMAND_REMOVE:
					usb_remove_pending_device(pending_command->source, 
											  pending_command->device_location, 
											  pending_command->completion_event);
					break;

				/* Monitor */
				case PENDING_DEVICE_COMMAND_SET_MONITOR:
					usb_handle_device_monitor_command(pending_command->device_location,
													  pending_command->monitor,
													  pending_command->completion_event);
					break;

				/* Invalid commnad */
				default:
					LOG_ERROR("Invalid pending device command type: %d", pending_command->type);
					break;
				}
			
				/* Refresh the monitored ports state */
				usb_update_monitored_devices();

				HEAP_FREE(pending_command);
			}
		
		}
		UNLOCK_PENDING_DEVICES();

		return 0;
	}

	/******************************************************************************
	 * usb_add_fds Function
	 *****************************************************************************/
	int usb_add_fds(fd_set * read_fds, fd_set * write_fds)
	{
		UNREFERENCED_PARAMETER(write_fds);

		int fd_count = 0;
		SOCKET endpoint_socket = INVALID_SOCKET;
		FOREACH(struct usb_device * dev, &g_device_list, struct usb_device *)
		{
			if (USB_DEVICE_STATE_ALIVE == dev->state)
			{
				if (PORTOK == PortGetCompletionSocketForEndpoint(&(dev->port), dev->info.ep_in, &endpoint_socket))
				{
					FD_SET(endpoint_socket, read_fds);
					fd_count++;
				}
				else
				{
					LOG_WIN32_ERROR("PortGetCompletionSocketForEndpoint");
				}
			}
		} ENDFOREACH

		return fd_count;
	}
#else
	/******************************************************************************
	 * usb_process Function
	 *****************************************************************************/
	int usb_process(void * unused)
	{
		/* Handle pending devices */
		LOCK_PENDING_DEVICES();
		if (false == g_pending_devices.IsEmpty())
		{
			size_t pending_commands_count = g_pending_devices.GetCount();
			for (size_t i = 0; i < pending_commands_count; i++)
			{
				PENDING_DEVICE_COMMAND * pending_command = g_pending_devices.RemoveHead();
				switch (pending_command->type)
				{
				/* Add */
				case PENDING_DEVICE_COMMAND_ADD:
					usb_add_pending_device(pending_command->source, 
										   pending_command->device_location, 
										   pending_command->monitor,
										   pending_command->completion_event);
					break;

				/* Remove */
				case PENDING_DEVICE_COMMAND_REMOVE:
					usb_remove_pending_device(pending_command->source, 
											  pending_command->device_location, 
											  pending_command->completion_event);
					break;

				/* Monitor */
				case PENDING_DEVICE_COMMAND_SET_MONITOR:
					usb_handle_device_monitor_command(pending_command->device_location,
													  pending_command->monitor,
													  pending_command->completion_event);
					break;

				/* Invalid commnad */
				default:
					DEBUG_PRINT_ERROR("Invalid pending device command type: %d", pending_command->type);
					break;
				}
			
				/* Refresh the monitored ports state */
				usb_update_monitored_devices();

				HEAP_FREE(pending_command);
			}
		
		}
		UNLOCK_PENDING_DEVICES();

		return 0;
	}
	/******************************************************************************
	 * usb_start_read Function
	 *****************************************************************************/
	int usb_start_read(struct usb_device * dev)
	{
		/* Make sure the read event isn't signaled */
		ResetEvent((dev->rx.overlapped).hEvent);

		/* Try to perform a read transfer */
		dev->rx.data_size = 0;


		if (COM_OK == com_plugin_transfer(&(dev->port),
									   TRUE,
									   dev->info.ep_in,
									   dev->rx.buffer,
									   DEVICE_RX_BUFFER_SIZE,//USB_MRU,
									   (LPDWORD)(&(dev->rx.data_size)),
									   NULL,
									   0,
									   &(dev->rx.overlapped)))
		{
			/* The caller will always wait on the returned handle and call usb_get_read_result,
			 * without caring if the transfer was completed synchronously or not,
			 * so we'll signal the event manually */
			SetEvent((dev->rx.overlapped).hEvent);
		}
		else
		{
			DWORD le = GetLastError();
			if (ERROR_IO_PENDING != le)
			{
				usb_handle_port_failure(dev);
				DEBUG_PRINT_WIN32_ERROR("PortPortTransfer");

				return -1;
			}
		}

		return (int)((dev->rx.overlapped).hEvent);
	}

	/******************************************************************************
	 * usb_get_read_result Function
	 *****************************************************************************/
	int usb_get_read_result(struct usb_device * dev, void ** buf, uint32_t * buf_data_size)
	{
		/* Check if the transfer didn't complete */
		if (0 == dev->rx.data_size)
		{
			if (COM_OK != com_plugin_get_transfer_result(&(dev->port),
													&(dev->rx.overlapped), 
													(LPDWORD)(&(dev->rx.data_size)), 
													FALSE))
			{
				usb_handle_port_failure(dev);
				DEBUG_PRINT_WIN32_ERROR("PortPortGetTransferResult");
				return -1;
			}
		}

		*buf = dev->rx.buffer;
		*buf_data_size = dev->rx.data_size;
	
		return 0;
	}
#endif /* USE_PORTDRIVER_SOCKETS */

/******************************************************************************
 * usb_send Function
 *****************************************************************************/


	void GetTXQElement(struct usb_device * dev, usb_device_tx_q_element& e, const unsigned char* buff)
{
	
	if (!dev->tx.pool.try_dequeue(e))
	{
		e.theOverLapped = new OVERLAPPED;
		memset(e.theOverLapped, 0, sizeof(OVERLAPPED));
		e.theOverLapped->hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	}
	e.buffer = (void*)buff;
}

void ReUseTXQElement(struct usb_device * dev, usb_device_tx_q_element& e){

	if (e.buffer)
		free(e.buffer);
	e.buffer = NULL;
	dev->tx.pool.enqueue(e);
}

int usb_send(struct usb_device * dev, const unsigned char * buf, int length)
{
	int iRet = -1;
	usb_device_tx_q_element e;
	GetTXQElement(dev, e, buf);

	/* Try to perform */
	DWORD dwBytesTransferred = 0;
	if (COM_OK == com_plugin_transfer(&(dev->port),
								   FALSE,
								   dev->info.ep_out,
								   (void *)buf,
								   length,
								   &dwBytesTransferred,
								   NULL,
								   0,
								   e.theOverLapped))
	{
		iRet = 0;
		ReUseTXQElement(dev, e);
	}
	else
	{

		/* Check if the transfer is pending */
		if (ERROR_IO_PENDING == GetLastError())
		{
			if (dev->tx.thread)
			{
				iRet = 0;
				dev->tx.q.enqueue(e);
			}
			else //write thread not started will do transfer result here
			{
				
				if (COM_OK == com_plugin_get_transfer_result(&(dev->port), e.theOverLapped, &dwBytesTransferred, TRUE))
				{
					iRet = 0;
				}
				else
				{
					DEBUG_PRINT_WIN32_ERROR("PortPortGetTransferResult");
					iRet = -1;
				}
				ReUseTXQElement(dev,e);
			}

		
			
		}
		else
		{

			DEBUG_PRINT_WIN32_ERROR("PortPortTransfer");
		}
	}

	/* TMP Patch:
	 * When disconneting a device, we count on the "read" transfer (done by the reading thread) to fail.
	 * For some reason, we've encountered situations where the above transfer fails, but the 
	 * "read" transfer doesn't (the PortPortTransfer just doesn't return). 
	 * For now, until we understand this issue, it's better to crash than to deal with the "stuck" overlapped
	 * transfer. */
	if (0 != iRet)
	{
		ExitProcess(1);
	}

	/* If needed - send a zero length packet */
	if ((0 == iRet) && (0 == (length % dev->info.tx_max_packet_size)))
	{
		DWORD dwEmptyBulk = 0;
		usb_device_tx_q_element ze;
		GetTXQElement(dev, ze,NULL);

		if (COM_OK == com_plugin_transfer(&(dev->port), FALSE, dev->info.ep_out, ze.buffer, 0, &dwEmptyBulk, NULL, 0, ze.theOverLapped))
		{
			iRet = 0;
			ReUseTXQElement(dev, ze);
		}
		else 
		{

			if ( dev->tx.thread)
			{
				iRet = 0;
				dev->tx.q.enqueue(ze);
			}
			else //write thread not started will do transfer result here
			{

				if (COM_OK == com_plugin_get_transfer_result(&(dev->port), ze.theOverLapped, &dwEmptyBulk, TRUE))
				{
					iRet = 0;
				}
				else
				{
					DEBUG_PRINT_WIN32_ERROR("PortPortGetTransferResult Zero");
					iRet = -1;
				}
				ReUseTXQElement(dev,ze);

			}
		}

	}

	return iRet;
}

/******************************************************************************
 * usb_get_serial Function
 *****************************************************************************/
const char *usb_get_serial(struct usb_device *dev)
{
	if (NULL == dev)
	{
		return NULL;
	}
	return dev->info.serial;
}

/******************************************************************************
 * usb_get_location Function
 *****************************************************************************/
uint32_t usb_get_location(struct usb_device *dev)
{
	if (NULL == dev)
	{
		return 0;
	}

	return dev->location;
}

/******************************************************************************
 * usb_get_pid Function
 *****************************************************************************/
uint16_t usb_get_pid(struct usb_device *dev)
{
	if (NULL == dev)
	{
		return 0;
	}

	return dev->port.wPID;
}

/******************************************************************************
 * usb_is_device_monitored Function
 *****************************************************************************/
int usb_is_device_monitored(struct usb_device *dev)
{
	if (FALSE == IS_VALID_DEVICE(dev))
	{
		return 0;
	}

	return (DEVICE_MONITOR_DISABLE != dev->monitor);
}

/******************************************************************************
 * usb_has_devices Function
 *****************************************************************************/
uint8_t usb_has_devices()
{
	/* We might get called from a thread other than the main thread, so we need
	 * to make sure there won't be any device changes while we check */
	LOCK_PENDING_DEVICES();

	/* We simply check if we have devices in our list, either present or monitored */
	uint8_t ret = collection_count(&g_device_list) > 0;

	UNLOCK_PENDING_DEVICES();
	return ret;
}

/******************************************************************************
 * usb_set_device_ready Function
 *****************************************************************************/
void usb_set_device_ready(struct usb_device *dev)
{
	if (IS_VALID_DEVICE(dev))
	{
		if (IS_VALID_HANDLE(dev->device_ready_event))
		{
			if (FALSE == SetEvent(dev->device_ready_event))
			{
				DEBUG_PRINT_WIN32_ERROR("SetEvent");
			}
		}
	}
}

/******************************************************************************
 * Internal Functions
 *****************************************************************************/
#ifdef USE_PORTDRIVER_SOCKETS
	/******************************************************************************
	 * rx_callback Function
	 *****************************************************************************/
	static void rx_callback(PORT_TRANSFER * ptTransfer)
	{
		struct usb_device * dev = (usb_device *)ptTransfer->pvCallbackContext;
		if (PORT_TRANSFER_STATUS_COMPLETED == ptTransfer->eStatus)
		{
			/* Push the new data up to the protocol (device) layer */
			uint32_t data_processed = 0;
			while (data_processed < ptTransfer->dwBytesTransferred)
			{
				data_processed += device_data_input(dev, 
													(unsigned char *)(ptTransfer->pvBuffer) + data_processed, 
													ptTransfer->dwBytesTransferred - data_processed);
			}
		
			/* Resubmit the transfer */
			if (PORTOK != PortSubmitTransfer(ptTransfer))
			{
				LOG_WIN32_ERROR("PortSubmitTransfer");
			}
		}
		else
		{
			switch (ptTransfer->eStatus)
			{
			/* Note: When the device is remove, we'll get an PORT_TRANSFER_STATUS_DEVICE_REMOVED
			 * error. On MCE port reset - we'll get PORT_TRANSFER_STATUS_CANCELLED */
			case PORT_TRANSFER_STATUS_CANCELLED:
			case PORT_TRANSFER_STATUS_DEVICE_REMOVED:
				LOG_ERROR("The device %d was removed unexpectedly", dev->location);
				dev->state = USB_DEVICE_STATE_DEAD;
				break;

			/* Unknown error */
			case PORT_TRANSFER_STATUS_FAILED:
				LOG_ERROR("The transfer on device %d has failed", dev->location);
				break;

			default:
				LOG_ERROR("Uknown transfer status: %d", ptTransfer->eStatus);
				break;
			}
		}
	}

	/******************************************************************************
	 * start_rx_loop Function
	 *****************************************************************************/
	static int start_rx_loop(struct usb_device *dev)
	{
		void * buffer = NULL;
		PORT_TRANSFER * transfer = NULL;

		/* Allocate a buffer for the transfer */
		buffer = HEAP_ALLOC(void, DEVICE_RX_BUFFER_SIZE);
		if (NULL == buffer)
		{
			return -1;
		}

		/* Allocate a new transfer */
		if (PORTOK != PortAllocateTransfer(&(dev->port), rx_callback, dev, &transfer))
		{
			LOG_WIN32_ERROR("PortAllocateTransfer");
			goto lblCleanup;
		}
		if (PORTOK != PortFillBulkOrInterruptTransfer(transfer, dev->info.ep_in, buffer, DEVICE_RX_BUFFER_SIZE))
		{
			LOG_WIN32_ERROR("PortFillBulkOrInterruptTransfer");
			goto lblCleanup;
		}


		/* Submit the transfer */
		if (PORTOK != PortSubmitTransfer(transfer))
		{
			LOG_WIN32_ERROR("PortSubmitTransfer");
			goto lblCleanup;
		}

		collection_add(&(dev->rx_transfers), transfer);
		return 0;

	lblCleanup:
		if (transfer)
		{
			if (PORTOK == PortFreeTransfer(transfer, FALSE))
			{
				transfer = NULL;
			}
		}

		if (buffer && (NULL == transfer))
		{
			HEAP_FREE(buffer);
		}

		return -1;
	}

	/******************************************************************************
	 * start_reading_from_device Function
	 *****************************************************************************/
	static int start_reading_from_device(struct usb_device * dev)
	{
		int rx_loops = NUM_RX_LOOPS;

		/* Start reading from the device, using parallel read loops */
		collection_init(&(dev)->rx_transfers);
		for (rx_loops = NUM_RX_LOOPS; rx_loops > 0; rx_loops--)
		{
			if (start_rx_loop(dev) < 0)
			{
				LOG_ERROR("Failed to start RX loop number %d", NUM_RX_LOOPS - rx_loops);
			}
		}

		/* Ensure we have at least 1 RX loop going */
		if (NUM_RX_LOOPS == rx_loops)
		{
			LOG_ERROR("Failed to start any RX loop for device %d", dev->location);
			return -1;
		}
		else if (rx_loops > 0)
		{
			LOG_ERROR("Failed to start all %d RX loops. Going on with %d loops. "
					  "This may have negative impact on device read speed.",
					  NUM_RX_LOOPS, NUM_RX_LOOPS - rx_loops);
		}
		else
		{
			LOG_TRACE("All %d RX loops started successfully", NUM_RX_LOOPS);
		}

		return 0;
	}

	/******************************************************************************
	 * usb_disconnect Function
	 *****************************************************************************/
	static void usb_disconnect(struct usb_device * dev, bool manual_remove)
	{
		/* Close the device's port.
		 * Note: This must be done before freeing the rx transfers, to make sure
		 * there are no pending overlapped operations. */
		bool port_was_closed = false;
		if (PORTOK == PortClosePort(&(dev->port)))
		{
			port_was_closed = true;
		}
		else
		{
			LOG_WIN32_ERROR("PortClosePort");
		}
	
		/* Free the rx transfers */
		if (IS_VALID_COLLECTION(&(dev->rx_transfers)))
		{
			if (port_was_closed)
			{
				FOREACH(PORT_TRANSFER * transfer, &(dev->rx_transfers), PORT_TRANSFER *)
				{
					void * transfer_buffer = transfer->pvBuffer;
					if (PORTOK == PortFreeTransfer(transfer, FALSE))
					{
						HEAP_FREE(transfer_buffer);
					}
				} ENDFOREACH
			}
			collection_free(&(dev->rx_transfers));
		}
	
		/* If wer're really removing the device, we'll cleanup 
		 * everything. But, if this was a removal of a monitored device, 
		 * we'll just reset it's state */
		if (manual_remove || (DEVICE_MONITOR_DISABLE == dev->monitor))
		{
			collection_remove(&g_device_list, dev);
			HEAP_FREE(dev);
		}
		else
		{
			/* Reset the device's state */
			SecureZeroMemory(&(dev->port), sizeof(dev->port));
			SecureZeroMemory(&(dev->info), sizeof(dev->info));
			dev->state = USB_DEVICE_STATE_WAITING_FOR_DEVICE;
		}
	}
#else
	/******************************************************************************
	 * usb_disconnect Function
	 *****************************************************************************/
	static void usb_disconnect(struct usb_device * dev, bool manual_remove)
	{
		/* Signal the read thread to stop event */
		if (IS_VALID_HANDLE(dev->rx.thread))
		{
			DEBUG_PRINT("Terminating read thread for device %d", dev->id);
			if (FALSE == SetEvent(dev->rx.thread_stop_event))
			{
				DEBUG_PRINT_WIN32_ERROR("SetEvent");
			}

			/* Wait for the read thread to finish */
			DWORD dwWaitResult = WaitForSingleObject(dev->rx.thread, READ_THREAD_SHUTDOWN_TIMEOUT);
			switch (dwWaitResult)
			{
			/* The thread has been terminated */
			case WAIT_OBJECT_0:
				DEBUG_PRINT("The read thread was terminated");
				break;

			/* Timeout */
			case WAIT_TIMEOUT:
				DEBUG_PRINT("Timeout while waiting for the read thread to terminate, killing the thread...");
				if (FALSE == TerminateThread(dev->rx.thread, 0))
				{
					DEBUG_PRINT_WIN32_ERROR("TerminateThread");
					return;
				}
				break;

			/* Error */
			default:
				DEBUG_PRINT_WIN32_ERROR("WaitForSingleObject");
				return;
			}
		}

		if (dev->tx.writeThreadStop)
		{
			SetEvent(dev->tx.writeThreadStop);
			usb_device_tx_q_element stop = { 0 };
			dev->tx.q.enqueue(stop);//

			
			
			if(WaitForSingleObject(dev->tx.thread,5000)!=WAIT_OBJECT_0)
			{
				DEBUG_PRINT_ERROR("waiting for write thread to stop failed");
			}
			else
			{
				DEBUG_PRINT_ERROR("waiting for write thread to stop success");
				usb_device_tx_q_element freePoll;
				while (dev->tx.pool.try_dequeue(freePoll))
				{
					if (freePoll.theOverLapped)
					{
						if (freePoll.theOverLapped->hEvent)
						{
							CloseHandle(freePoll.theOverLapped->hEvent);
						}
						delete  (freePoll.theOverLapped);
					}
					
				}
				DEBUG_PRINT_ERROR("deliting pool finished");
			}


		}


		/* Cleanup this device's instance related resources */
		if (COM_OK != com_plugin_close(&(dev->port)))
		{
			DEBUG_PRINT_WIN32_ERROR("PortClosePort");
		}
		SAFE_CLOSE_SOCKET(dev->rx.data_events_socket);
		SAFE_CLOSE_HANDLE(dev->rx.thread);

		/* If wer're really removing the device, we'll cleanup 
		 * everything. But, if this was a removal of a monitored device, 
		 * we'll just reset it's state */
		if (manual_remove || (DEVICE_MONITOR_DISABLE == dev->monitor))
		{
			usb_free_device(dev);
		}
		else
		{
			/* Reset the device's state */
			SecureZeroMemory(&(dev->port), sizeof(dev->port));
			SecureZeroMemory(&(dev->info), sizeof(dev->info));
			dev->state = USB_DEVICE_STATE_WAITING_FOR_DEVICE;
		}
	}

	/******************************************************************************
	 * usb_read_thread_proc Function
	 *****************************************************************************/
	static DWORD WINAPI usb_read_thread_proc(void * context)
	{
		CHAR cFlag = 1;

		usb_device * dev = (usb_device *)context;

		HANDLE ahEvents[2];
		ahEvents[READ_WAIT_EVENTS_ARR_STOP_EVENT] = dev->rx.thread_stop_event;
		ahEvents[READ_WAIT_EVENTS_ARR_DEVICE_EVENT] = (HANDLE)usb_start_read(dev);
		if (FALSE == IS_VALID_HANDLE(ahEvents[READ_WAIT_EVENTS_ARR_DEVICE_EVENT]))
		{
			DEBUG_PRINT_ERROR("usb_start_read has failed");
			return 0;
		}

		bool should_stop = false;
		DWORD wait_result = WAIT_FAILED;
		int socket_result = 0;
		while (false == should_stop)
		{
			wait_result = WaitForMultipleObjects(STATIC_ARRAY_SIZE(ahEvents), ahEvents, FALSE, READ_WAIT_TIMEOUT);
			switch (wait_result)
			{
			/* Stop Event */
			case WAIT_OBJECT_0 + READ_WAIT_EVENTS_ARR_STOP_EVENT:
				DEBUG_PRINT("Stop event was signaled");
				should_stop = true;
				break;

			/* Device data */
			case WAIT_OBJECT_0 + READ_WAIT_EVENTS_ARR_DEVICE_EVENT:
				/* Notify the main thread we have new data and wait for it to process it. 
				 * Note: It doesn't matter what we send and receive, but it should be one 
				 * byte. */
				socket_result = send(dev->rx.data_events_socket, &cFlag, sizeof(cFlag), 0);
				if (sizeof(cFlag) == socket_result)
				{
					socket_result = recv(dev->rx.data_events_socket, &cFlag, sizeof(cFlag), 0);
					if (sizeof(cFlag) == socket_result)
					{
						/* Start another read */
						ahEvents[READ_WAIT_EVENTS_ARR_DEVICE_EVENT] = (HANDLE)usb_start_read(dev);
						if (FALSE == IS_VALID_HANDLE(ahEvents[READ_WAIT_EVENTS_ARR_DEVICE_EVENT]))
						{
							DEBUG_PRINT_ERROR("usb_start_read has failed socket_result:%d", socket_result);
							should_stop = true;
						}

					}
					else
					{
						if (0 == socket_result)
						{
							DEBUG_PRINT("The device socket was closed");
						}
						else
						{
							DEBUG_PRINT_WSA_ERROR("recv");
						}
					
						should_stop = true;
					}
				}
				else
				{
					if (0 == socket_result)
					{
						DEBUG_PRINT("The device socket was closed");
					}
					else
					{
						DEBUG_PRINT_WSA_ERROR("send");
					}
				
					should_stop = true;
				}

				break;

			/* Timeout (ignored) */
			case WAIT_TIMEOUT:
				break;

			/* Error */
			default:
				DEBUG_PRINT_WIN32_ERROR("WaitForMultipleObjects");
				should_stop = true;
				break;
			}
		}

		return 0;
	}

	HANDLE g_hTransferresultMutex = CreateMutexA(NULL, FALSE, "");


	static DWORD WINAPI usb_write_thread_proc(void * context)
	{
		usb_device * dev = (usb_device *)context;
		while (true)
		{
			usb_device_tx_q_element e = { 0 }; 
			dev->tx.q.wait_dequeue(e);

			if (WaitForSingleObject(dev->tx.writeThreadStop, 0) == WAIT_OBJECT_0)
			{
				DEBUG_PRINT("usb_write_thread_proc signalled to stop");
				break;

			}
			WaitForSingleObject(g_hTransferresultMutex, INFINITE);
			if (e.theOverLapped != NULL)
			{
				DWORD dwBytesTransferred = 0;
				if (COM_OK == com_plugin_get_transfer_result(&(dev->port), e.theOverLapped, &dwBytesTransferred, TRUE))
				{

				}
				else
				{
					DEBUG_PRINT_WIN32_ERROR("PortPortGetTransferResult");
				}
				ReUseTXQElement(dev, e);
			}
			ReleaseMutex(g_hTransferresultMutex);

			
		}
		/* cleaning what remains in q*/
		usb_device_tx_q_element e = { 0 };
		while (dev->tx.q.try_dequeue(e))
		{
			if(e.buffer)
				free((void *)e.buffer);
			if(e.theOverLapped && e.theOverLapped->hEvent)
				CloseHandle(e.theOverLapped->hEvent);
			if (e.theOverLapped)
				delete e.theOverLapped;

		}
	
		DEBUG_PRINT("usb_write_thread_proc ended");
		return 0;

	}
	/******************************************************************************
	* usb_start_write_thread Function
	*****************************************************************************/

	struct temp
	{
		void* buffer;
	};

	static int usb_start_write_thread(usb_device * dev)
	{

		dev->tx.writeThreadStop = CreateEventA(NULL, FALSE, FALSE, "");
		dev->tx.thread = CREATE_THREAD(usb_write_thread_proc, dev);
		
		if (NULL == dev->tx.thread)
		{
			DEBUG_PRINT_WIN32_ERROR("_beginthreadtx");
			goto lblCleanup;
		}



		return 0;
		lblCleanup:
		return -1;
	}


	/******************************************************************************
	 * usb_start_read_thread Function
	 *****************************************************************************/
	static int usb_start_read_thread(usb_device * dev)
	{
		/* Connect the rx events sockets, which will be used to communicate with 
		 * the main thread */
		dev->rx.data_events_socket = ConnectSocket(LOCALHOST_ADDR, usbmuxd_GetDevicesPort(), CONNECT_SOCKET_TYPE_TCP);
		if (INVALID_SOCKET == dev->rx.data_events_socket)
		{
			DEBUG_PRINT_ERROR("Failed to connect to the devices socket");
			return -1;
		}

		/* Disable buffering of small pakcets */
		BOOL tcp_no_delay = TRUE;
		if (0 != setsockopt(dev->rx.data_events_socket, IPPROTO_TCP, TCP_NODELAY, (char *)&tcp_no_delay, sizeof(tcp_no_delay)))
		{
			DEBUG_PRINT_WSA_ERROR("setsockopt");
			goto lblCleanup;
		}
	
		/* Send our device's id to the main thread */
		if (sizeof(dev->id) != send(dev->rx.data_events_socket, (char *)&(dev->id), sizeof(dev->id), 0))
		{
			DEBUG_PRINT_WSA_ERROR("send");
			goto lblCleanup;
		}

		/* Create the proxy thread's stop event (will be signaled in order to
		 * terminate the thread) */
		if (IS_VALID_HANDLE(dev->rx.thread_stop_event))
		{
			(void)ResetEvent(dev->rx.thread_stop_event);
		}
		else
		{
			dev->rx.thread_stop_event = CreateEvent(NULL, FALSE, FALSE, NULL);
			if (FALSE == IS_VALID_HANDLE(dev->rx.thread_stop_event))
			{
				DEBUG_PRINT_WIN32_ERROR("CreateEvent");
				goto lblCleanup;
			}
		}
	
		/* Start the proxy thread */
		dev->rx.thread = CREATE_THREAD(usb_read_thread_proc, dev);
		if (NULL == dev->rx.thread)
		{
			DEBUG_PRINT_WIN32_ERROR("_beginthreadex");
			goto lblCleanup;
		}

		


		return 0;

	lblCleanup:
		SAFE_CLOSE_SOCKET(dev->rx.data_events_socket);
		SAFE_CLOSE_HANDLE(dev->rx.thread_stop_event);
	
		return -1;	
	}
#endif /* USE_PORTDRIVER_SOCKETS */

/******************************************************************************
 * usb_update_monitored_devices Function
 *****************************************************************************/
static void usb_update_monitored_devices()
{
	/* Check if we have devices to monitor */
	bool have_monitored_ports = 0;
	FOREACH(struct usb_device * dev, &g_device_list, struct usb_device *)
	{
		if (DEVICE_MONITOR_DISABLE != dev->monitor)
		{
			have_monitored_ports = true;
			break;
		}
	} ENDFOREACH

	/* If we have ports to monitor, register a port notification callback if we hadn't already done it */
	if (have_monitored_ports)
	{
		if (NULL == g_port_notification_callback_cookie)
		{
			/* Regiter a port change notification callback */
			if (COM_OK != com_plugin_register_com_notification(usb_port_change_callback, NULL, &g_port_notification_callback_cookie))
			{
				DEBUG_PRINT_WIN32_ERROR("PortRegisterPortNotification");
			}
		}
	}
	/* If we don't have ports to monitor - remove our callback */
	else
	{
		if (g_port_notification_callback_cookie)
		{
			/* Unregister our port change notification callback */
			if (COM_OK != com_plugin_unregister_com_notification(g_port_notification_callback_cookie))
			{
				DEBUG_PRINT_WIN32_ERROR("PortUnRegisterPortNotification");
			}

			g_port_notification_callback_cookie = NULL;
		}
	}
}

/******************************************************************************
 * usb_port_change_callback Function
 *****************************************************************************/
static void usb_port_change_callback(void * context, const DWORD event, const char * port_name)
{
	/* Determine if this is a device arrival or removal */
	PENDING_DEVICE_COMMAND_TYPE command_type;
	if (DBT_DEVICEARRIVAL == event)
	{
		command_type = PENDING_DEVICE_COMMAND_ADD;
	}
	else if (DBT_DEVICEREMOVECOMPLETE == event)
	{
		command_type = PENDING_DEVICE_COMMAND_REMOVE;
	}
	else
	{
		DEBUG_PRINT_ERROR("Invalid event: %u", event);
		return;
	}

	/* Parse the port number, which is used as the device's location */
	uint32_t device_location = 0;
	sscanf_s(port_name, MCE_PORT_NAME_FORMAT, &device_location);

	/* Queue the device change to the main thread to process */
	usb_append_device_command(command_type, PENDING_DEVICE_COMMAND_SOURCE_MONITOR, device_location, NULL, DEVICE_MONITOR_DISABLE);
}

/******************************************************************************
 * usb_handle_device_monitor_command Function
 *****************************************************************************/
static int usb_handle_device_monitor_command(uint32_t					device_location,
											 enum device_monitor_state	monitor,
											 HANDLE						completion_event)
{
	struct usb_device * usb_dev = NULL;
	int ret = 0;

	/* Find the device */
	FOREACH(struct usb_device * dev, &g_device_list, struct usb_device *)
	{
		if (device_location == dev->location)
		{
			usb_dev = dev;
		}
	} ENDFOREACH

	if (usb_dev)
	{
		/* If the device is already monitored, we'll ignore "monitor once" request,
		 * which is only used by the preflight thread, so we won't lose the "always monitor"
		 * setting. */
		if (FALSE == ((DEVICE_MONITOR_ALWAYS == usb_dev->monitor) && (DEVICE_MONITOR_ONCE == monitor)))
		{
			if ((DEVICE_MONITOR_DISABLE == monitor) && (USB_DEVICE_STATE_WAITING_FOR_DEVICE == usb_dev->state))
			{
				usb_free_device(usb_dev);
			}
			else
			{
				usb_dev->monitor = monitor;
			}
		}		
	}
	else
	{
		/* If we want to monitor a device, but it hasn't been added yet - add it */
		if (DEVICE_MONITOR_ALWAYS == monitor)
		{
			int ret = usb_add_pending_device(PENDING_DEVICE_COMMAND_SOURCE_MANUAL, device_location, monitor, NULL);
		}
		else
		{
			DEBUG_PRINT("Couldn't find a device for port: %u", device_location);
			ret = -1;
		}
	}

	/* Signal we're done */
	if (0 == ret)
	{
		if (IS_VALID_HANDLE(completion_event))
		{
			(void)SetEvent(completion_event);
		}
	}
	
	return ret;
}

/******************************************************************************
 * usb_append_device_command Function
 *****************************************************************************/
static void usb_append_device_command(PENDING_DEVICE_COMMAND_TYPE	type, 
									  PENDING_DEVICE_COMMAND_SOURCE source,
									  uint32_t						device_location,
									  void							* completion_event,
									  enum device_monitor_state		monitor_device)
{
	PENDING_DEVICE_COMMAND * command = HEAP_ALLOC(PENDING_DEVICE_COMMAND, sizeof(PENDING_DEVICE_COMMAND));
	command->type = type;
	command->source = source;
	command->device_location = device_location;
	command->completion_event = (HANDLE)completion_event;
	command->monitor = monitor_device;

	LOCK_PENDING_DEVICES();
	g_pending_devices.AddTail(command);
	UNLOCK_PENDING_DEVICES();
}

/******************************************************************************
 * usb_add_pending_device Function
 *****************************************************************************/
static int usb_add_pending_device(PENDING_DEVICE_COMMAND_SOURCE source, 
								  uint32_t						device_location,
								  enum device_monitor_state		monitor,
								  HANDLE						device_ready_event)
{


	
	struct usb_device * usb_dev = NULL;
	bool is_existing_device = false;
	bool was_port_opened = false;
	bool was_device_added = false;

	/* Check for an existing, monitored, dead, device. If we won't one, we'll assume
	 * its a new device */
	FOREACH(struct usb_device * dev, &g_device_list, struct usb_device *)
	{
		if (device_location == dev->location)
		{
			if (IS_VALID_DEVICE(dev))
			{
				DEBUG_PRINT_ERROR("The device at location %u already exists", device_location);
				usb_report_device_already_exists(dev);
				return -1;
			}

			usb_dev = dev;
			is_existing_device = true;
		}
	} ENDFOREACH

	char port_name[MAX_PATH] = {'\0'};
	StringCchPrintfA(port_name, MAX_PATH, MCE_PORT_NAME_FORMAT, device_location);

	if (NULL == usb_dev)
	{
		/* If we got here because we've received a device change notfication,
		 * but couldn't find a monitored device, we'll ignore it */
		if (PENDING_DEVICE_COMMAND_SOURCE_MONITOR == source)
		{
			DEBUG_PRINT("Ignoring device: %s", port_name);
			return 0;
		}

		DEBUG_PRINT("Adding a pending device: %s", port_name);
		usb_dev = new usb_device;

		#ifndef USE_PORTDRIVER_SOCKETS
			/* Reset data_events_socket to INVALID_SOCKET (SAFE_CLOSE_SOCKET doesn't
			 * check for NULL) */
			usb_dev->rx.data_events_socket = INVALID_SOCKET;
		#endif
	}
	else
	{
		DEBUG_PRINT("Detected a monitored device %s", port_name);
	}

	/* Set the device's info (which will be used by the cleanup if we'll fail) */
	if (false == is_existing_device)
	{
		usb_dev->id = g_next_usb_device_id++;
		usb_dev->location = device_location;
	}

	/* Try to open the port */
	was_port_opened = (COM_OK == com_plugin_open_by_name(port_name, &(usb_dev->port)));
	if (false == was_port_opened)
	{
		if (DEVICE_MONITOR_ALWAYS == monitor)
		{
			DEBUG_PRINT("Failed to open port, starting to monitor port");
			usb_dev->state = USB_DEVICE_STATE_WAITING_FOR_DEVICE;
		}
		else
		{
			DEBUG_PRINT_WIN32_ERROR("PortOpenPortByName");
			goto lblCleanup;
		}
	}

	/* Initialize the device's struct */
	if (false == is_existing_device)
	{
		usb_dev->monitor = monitor;
		#ifndef USE_PORTDRIVER_SOCKETS
			usb_dev->rx.buffer = HEAP_ALLOC(BYTE, DEVICE_RX_BUFFER_SIZE);
			usb_dev->rx.overlapped.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
			if (FALSE == IS_VALID_HANDLE(usb_dev->rx.overlapped.hEvent))
			{
				DEBUG_PRINT_WIN32_ERROR("CreateEvent");
				goto lblCleanup;
			}
		#endif

		collection_add(&g_device_list, usb_dev);
	}
	else
	{
		/* If the device should only be monitored once - reset its monitor state */
		if (DEVICE_MONITOR_ONCE == usb_dev->monitor)
		{
			usb_dev->monitor = DEVICE_MONITOR_DISABLE;
		}
	}

	/* If we didn't manage to open the port, we can't continue with initializing the port */
	if (false == was_port_opened)
	{
		return 0;
	}

	/* Verify the device's vid\pid */
	if ((VID_APPLE != usb_dev->port.wVID) ||
		(usb_dev->port.wPID < PID_RANGE_LOW) ||
		(usb_dev->port.wPID > PID_RANGE_MAX))
	{
		if (PENDING_DEVICE_COMMAND_SOURCE_MONITOR == source)
		{
			DEBUG_PRINT("Got an unsupported vid\\pid: %x %x, ignoring it", usb_dev->port.wVID, usb_dev->port.wPID);
			goto lblCleanup;
		}

		if (DEVICE_MONITOR_DISABLE != monitor)
		{
			DEBUG_PRINT("Got an unsupported vid\\pid: %x %x, starting to monitor port", usb_dev->port.wVID, usb_dev->port.wPID);
			return 0;
		}

		DEBUG_PRINT_ERROR("Device added is not an apple, mux supporting, device");
		goto lblCleanup;
	}

	/* Initialize the device's configuration */
	if (false == usb_configure_device(usb_dev))
	{
		DEBUG_PRINT_ERROR("Failed to configure device");
		if (is_existing_device && (DEVICE_MONITOR_ALWAYS == usb_dev->monitor))
		{
			return 0;
		}

		goto lblCleanup;
	}

	/* Store the caller supplied event, which will be signaled when the device
	 * becomes ready and visible */
	if (IS_VALID_HANDLE(device_ready_event))
	{
		usb_dev->device_ready_event = device_ready_event;
	}
	



	/* Let the device module know about the new device */
	if (device_add(usb_dev) < 0)
	{
		DEBUG_PRINT_ERROR("device_add has failed");
		goto lblCleanup;
	}

	
	was_device_added = 1;

	#ifdef USE_PORTDRIVER_SOCKETS
		/* Start reading from the device */
		if (start_reading_from_device(usb_dev) < 0)
		{
			goto lblCleanup;
		}
	#else
		/* Start the reading thread */
		
		if (usb_start_read_thread(usb_dev) < 0)
		{
			DEBUG_PRINT_ERROR("Failed to start the read thread");
			goto lblCleanup;
		}

		if (usb_dev->port.bTurbo)
		{
			if (usb_start_write_thread(usb_dev) < 0)
			{
				DEBUG_PRINT_ERROR("Failed to start the write thread");
				goto lblCleanup;
			}
		}
		else
			DEBUG_PRINT("usb_start_write_thread skipped - turbo mode disabled (should be done for warehouse with mulltiple transaction)");

	#endif
	
	usb_dev->state = USB_DEVICE_STATE_ALIVE;
	return 0;

lblCleanup:
	bool should_keep_device = is_existing_device && (DEVICE_MONITOR_ALWAYS == usb_dev->monitor);
	
	/* Tell the device layer to cleanup */
	if (was_device_added) {
		device_remove(usb_dev);
	} else if (false == should_keep_device) {
		device_add_failed(usb_dev);
	}

	if (should_keep_device)
	{
		/* If this is an already monitored port, we don't want to remove it, just skip it */
		usb_disconnect(usb_dev, false);
		return 0;
	}

	usb_disconnect(usb_dev, true);
	return -1;
}

static void usb_report_device_already_exists(struct usb_device * dev)
{
	/* To prevent reporting this error during ios 7 trust, we'll skip devices 
	 * that are during trust */
	if (DEVICE_MONITOR_ONCE == dev->monitor)
	{
		return;
	}
	if (device_is_initializing(dev))
	{
		return;
	}

	struct device_info deviceInfo { 0 };
	deviceInfo.id = dev->id;
	deviceInfo.location = usb_get_location(dev);
	deviceInfo.serial = usb_get_serial(dev);
	deviceInfo.pid = usb_get_pid(dev);
	client_device_error_already_exits(&deviceInfo);
}

/******************************************************************************
 * usb_remove_pending_device Function
 *****************************************************************************/
static int usb_remove_pending_device(PENDING_DEVICE_COMMAND_SOURCE	source, 
									 uint32_t						device_location,
									 HANDLE							remove_completed_event)
{
	/* Get the usb_device struct for the port handle */
	struct usb_device * usb_dev = NULL;
	FOREACH(struct usb_device * tdev, &g_device_list, struct usb_device *)
	{
		if (device_location == tdev->location)
		{
			usb_dev = tdev;
			break;
		}
	} ENDFOREACH
	if (NULL == usb_dev)
	{
		if (PENDING_DEVICE_COMMAND_SOURCE_MONITOR == source)
		{
			DEBUG_PRINT("Ignoring port %u removal", device_location);
			return 0;
		}

		DEBUG_PRINT_ERROR("Unknown port handle");
		return -1;
	}

	DEBUG_PRINT("Removing a pending device");
	device_remove(usb_dev);
	usb_disconnect(usb_dev, (PENDING_DEVICE_COMMAND_SOURCE_MANUAL == source));

	/* Signal the device was removed */
	if (IS_VALID_HANDLE(remove_completed_event))
	{
		(void)SetEvent(remove_completed_event);
	}

	return 0;
}

/******************************************************************************
 * usb_configure_device Function
 *****************************************************************************/
static bool usb_configure_device(struct usb_device * usb_dev)
{
	bool bRet = false;
	DWORD cbConfigurationDescSize = 0;
	BYTE * pcConfigurationDesc = NULL;
	PUSB_CONFIGURATION_DESCRIPTOR ptConfigurationDesc = NULL;

	/* Get the device descriptor */
	USB_DEVICE_DESCRIPTOR tDeviceDesc = { 0 };
	if (COM_OK != com_pluging_get_device_descriptor(&(usb_dev->port), (BYTE *)&tDeviceDesc))
	{
		DEBUG_PRINT_WIN32_ERROR("PortGetPortDeviceDescriptor");
		goto lblCleanup;
	}
	
	/* Get the device's serial number (which is also its udid) */
	if (COM_OK != com_plugin_get_ascii_string_descriptor(&(usb_dev->port),
												   tDeviceDesc.iSerialNumber, 
												   usb_dev->info.serial, 
												   STATIC_ARRAY_SIZE(usb_dev->info.serial)))
	{
		DEBUG_PRINT_WIN32_ERROR("PortGetPortAsciiStringDescriptor");
		goto lblCleanup;
	}
	DEBUG_PRINT("Device UDID: %s", usb_dev->info.serial);
	/* Based on the original usbmuxd, the right configuration for mux is always 
	 * the last one. */
	
	/* Allocate a buffer for the configuration descriptor */
	com_plugin_get_configuration_descriptor(&(usb_dev->port),
									   tDeviceDesc.bNumConfigurations - 1, 
									   NULL, 
									   &cbConfigurationDescSize);
	if (0 == cbConfigurationDescSize)
	{
		DEBUG_PRINT_WIN32_ERROR("PortGetPortConfigurationDescriptor");
		goto lblCleanup;
	}
	pcConfigurationDesc = HEAP_ALLOC(BYTE, cbConfigurationDescSize);

	/* Get the configuration descriptor */
	if (COM_OK != com_plugin_get_configuration_descriptor(&(usb_dev->port),
													 tDeviceDesc.bNumConfigurations - 1, 
													 pcConfigurationDesc, 
													 &cbConfigurationDescSize))
	{
		DEBUG_PRINT_WIN32_ERROR("PortGetPortConfigurationDescriptor");
		goto lblCleanup;
	}
	PUSB_CONFIGURATION_DESCRIPTOR ptConfigurationDescHeader = (PUSB_CONFIGURATION_DESCRIPTOR)pcConfigurationDesc;
	
	/* Select the last configuration */
	if (COM_OK != com_plugin_select_configuration(&(usb_dev->port), ptConfigurationDescHeader->bConfigurationValue))
	{
		DEBUG_PRINT_ERROR("Failed to select configuration %u. Last error: %u", ptConfigurationDescHeader->bConfigurationValue);
		goto lblCleanup;
	}
	DEBUG_PRINT("Configuration %u was selected", ptConfigurationDescHeader->bConfigurationValue);

	/* Look for the mux interface */
	if (false == usb_configure_mux_interface(usb_dev, pcConfigurationDesc))
	{
		DEBUG_PRINT("Failed to find a valid mux interface ");
		goto lblCleanup;
	}
	
	bRet = true;

lblCleanup:
	if (pcConfigurationDesc)
	{
		HEAP_FREE(pcConfigurationDesc);
	}

	return bRet;
}

/******************************************************************************
 * usb_configure_mux_interface Function
 *****************************************************************************/
static bool usb_configure_mux_interface(struct usb_device * usb_dev, unsigned char * config_desc)
{
	bool bFoundInterface = false;

	/* Look for the mux interface */
	PUSB_COMMON_DESCRIPTOR ptCommonHeader = (PUSB_COMMON_DESCRIPTOR)(config_desc + sizeof(USB_CONFIGURATION_DESCRIPTOR));
	while (ptCommonHeader->bLength > 0)
	{
		PUSB_INTERFACE_DESCRIPTOR ptInterfaceDesc = NULL;
		PUSB_ENDPOINT_DESCRIPTOR pEndPointDesc = NULL;

		switch (ptCommonHeader->bDescriptorType)
		{
		/* Interface */
		case USB_INTERFACE_DESCRIPTOR_TYPE:
			ptInterfaceDesc = (PUSB_INTERFACE_DESCRIPTOR)ptCommonHeader;

			if ((INTERFACE_CLASS == ptInterfaceDesc->bInterfaceClass) &&
				(INTERFACE_SUBCLASS == ptInterfaceDesc->bInterfaceSubClass) &&
				(INTERFACE_PROTOCOL == ptInterfaceDesc->bInterfaceProtocol))
			{
				/* The original usbmuxd verifies there are only two endpoints */
				if (2 == ptInterfaceDesc->bNumEndpoints)
				{
					/* We've found our interface */
					DEBUG_PRINT("Found mux interface: %u", ptInterfaceDesc->bInterfaceNumber);
					bFoundInterface = true;
				}
				else
				{
					DEBUG_PRINT_ERROR("Invalid number of endpoints for interface %u: %u",
							  ptInterfaceDesc->bInterfaceNumber,
							  ptInterfaceDesc->bNumEndpoints);
				}
			}

			break;

		/* Endpoint*/
		case USB_ENDPOINT_DESCRIPTOR_TYPE:
			if (bFoundInterface)
			{
				pEndPointDesc = (PUSB_ENDPOINT_DESCRIPTOR)ptCommonHeader;
				if (USB_ENDPOINT_TYPE_BULK == pEndPointDesc->bmAttributes)
				{
					/* Read EP */
					if (USB_ENDPOINT_DIRECTION_IN(pEndPointDesc->bEndpointAddress))
					{
						usb_dev->info.ep_in = pEndPointDesc->bEndpointAddress;
						DEBUG_PRINT("Found read EP: 0x%x", usb_dev->info.ep_in);
					}
					/* Write EP */
					else
					{
						usb_dev->info.ep_out = pEndPointDesc->bEndpointAddress;
						usb_dev->info.tx_max_packet_size = pEndPointDesc->wMaxPacketSize;
						DEBUG_PRINT("Found write EP: 0x%x, max packet size: %u", usb_dev->info.ep_out, usb_dev->info.tx_max_packet_size);
					}

					if ((usb_dev->info.ep_in) && (usb_dev->info.ep_out))
					{
						#ifdef USE_PORTDRIVER_SOCKETS
							/* Create a transfer completion notification socket for the in endpoint
							 * (to allow to pass a socket to the main "select" loop) */
							if (PORTOK != PortCreateCompletionSocketsForEndpoints(&(usb_dev->port), 1, usb_dev->info.ep_in))
							{
								LOG_WIN32_ERROR("PortCreateCompletionSocketsForEndpoints");
								return false;
							}
						#endif

						return true;
					}
				}
			}

			break;
		}

		ptCommonHeader = (PUSB_COMMON_DESCRIPTOR)(((BYTE *)ptCommonHeader) + ptCommonHeader->bLength);
	}

	return false;
}

/******************************************************************************
 * usb_handle_port_failure Function
 *****************************************************************************/
static void usb_handle_port_failure(struct usb_device * dev)
{
	bool bDeviceRemoved = true;

	/* If the device was removed - schedule a remove command
	 * Note: We'll use the "monitor" source so we won't remove
	 * monitored devices).
	 * Note: When the device is remove, we'll get an ERROR_GEN_FAILURE error.
	 * On MCE port reset - we'll get ERROR_OPERATION_ABORTED */
	DWORD dwLastError = GetLastError();
	if ((ERROR_GEN_FAILURE == dwLastError) || (ERROR_OPERATION_ABORTED == dwLastError))
	{
		/* NOTE: Resetting the device at this point seems to be problematic, and cause the driver's 
		 * reset_device to lock for about 20 seconds. In some situations, for some reason, 
		 * this will also fail future write transfers. 
		 * For now we'll disable this fix. */

		/* ERROR_GEN_FAILURE might be caused by other error situations, like when the endpoint
		 * was stalled. It seems that on some device, and error might occure from time to time.
		 * In these situations, because we're deleting the device when it wasn't really removed -
		 * we won't be able to talk to the device without the user disconnecting and reconnecting
		 * the device. To solve that, we'll try to reset the device:
		 * - If the device was really removed, PortResetDevice will fail, and no harm was done.
		 * - If the device is still present, resetting the device will "emulate" disconnecting and reconnecting,
		 * letting usbmuxd's users know they need to restart their business. */
		/*if (ERROR_GEN_FAILURE == dwLastError)
		{
			if (PORTOK == PortResetDevice(&(dev->port)))
			{
				LOG_ERROR("device %d: Transfer has failed with ERROR_GEN_FAILURE but PortResetDevice has succeeded",
						  dev->location);
				bDeviceRemoved = false;
			}
		}*/

		if (bDeviceRemoved)
		{
			DEBUG_PRINT_ERROR("The device %d was removed unexpectedly", dev->location);
		}
		
		usb_append_device_command(PENDING_DEVICE_COMMAND_REMOVE,
								  PENDING_DEVICE_COMMAND_SOURCE_MONITOR,
								  dev->location,
								  NULL,
								  DEVICE_MONITOR_DISABLE);
	}
}

/******************************************************************************
 * usb_free_device Function
 *****************************************************************************/
static void usb_free_device(struct usb_device *dev)
{
	/* Release the device's resources */
	CloseHandle(dev->rx.thread_stop_event);

	HEAP_FREE(dev->rx.buffer);
	CloseHandle(dev->rx.overlapped.hEvent);

	collection_remove(&g_device_list, dev);
	delete (dev);
}