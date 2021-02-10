/******************************************************************************
 * usbmuxd.cpp
 *****************************************************************************/
/******************************************************************************
 * Includes
 *****************************************************************************/
#include "stdafx.h"
#include "usbmuxd.h"
#include "usbmuxd_internal.h"
#include "client.h"
#include "device.h"
#include "usb.h"
#include <libimobiledevice\libimobiledevice.h>

/* MCE Includes */
#include "windowsutil.h"
#include "SocketUtil.h"
#include "debugprint.h"

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

/******************************************************************************
 * Exported Functions
 *****************************************************************************/
/******************************************************************************
 * usbmuxd_Start Function
 *****************************************************************************/
USBMUXD_API BOOL usbmuxd_Start(WORD wPort, DWORD dwHubAddress, LPCWSTR pPluginPath)
{
	if (IS_USBMUXD_RUNNING())
	{
		DEBUG_PRINT_ERROR("usbmuxd was already started");
		return FALSE;
	}

	/* Enable libimobiledevice's debug logging on debug builds */
	#ifdef _DEBUG
		RedirectConsoleToDebug();
		idevice_set_debug_level(1);
	#endif*/

	/* Initialize Winsock */
	WSADATA wsaData = { 0 };
	int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != NO_ERROR)
	{
		DEBUG_PRINT_ERROR("WSAStartup has failed: %d", iResult);
		return FALSE;
	}

	/* Initiailzie usbmuxd's modules */
	//LOG_TRACE("Initializing usbmuxd");
	client_init();
	device_init();
	if (usb_init(dwHubAddress, pPluginPath) < 0)
	{
		return FALSE;
	}
	
	/* Initialize our global context */
	SecureZeroMemory((void *)&g_tContext, sizeof(g_tContext));
	g_tContext.hShutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	g_tContext.hReadyEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	g_tContext.wClientsPort = wPort;
	g_tContext.wDevicesPort = 0;
	if (FALSE == IS_VALID_HANDLE(g_tContext.hShutdownEvent) ||
		FALSE == IS_VALID_HANDLE(g_tContext.hReadyEvent))
	{
		DEBUG_PRINT_WIN32_ERROR("CreateEvent");
		goto lblCleanup;
	}

	/* Start the main thread */
	g_hMainThread = CREATE_THREAD(MainThreadProc, &g_tContext);
	if (FALSE == IS_VALID_HANDLE(g_hMainThread))
	{
		DEBUG_PRINT_WIN32_ERROR("_beginthreadex");
		goto lblCleanup;
	}

	/* Wait for the main thread to finish its initialization */
	if (WAIT_OBJECT_0 != WaitForSingleObject(g_tContext.hReadyEvent, INIT_TIMEOUT))
	{
		DEBUG_PRINT_ERROR("Failed to wait for the main thread to finish its initialization");
		goto lblCleanup;
	}

	return TRUE;

lblCleanup:
	usbmuxd_Shutdown();
	return FALSE;
}

/******************************************************************************
 * usbmuxd_Shutdown Function
 *****************************************************************************/
USBMUXD_API BOOL usbmuxd_Shutdown()
{
	/* Verify our state is valid */
	if (FALSE == IS_USBMUXD_RUNNING())
	{
		DEBUG_PRINT_ERROR("usbmuxd wasn't started");
		return FALSE;
	}
	if (FALSE == IS_VALID_HANDLE(g_tContext.hShutdownEvent))
	{
		DEBUG_PRINT_ERROR("Shutdown event is invalid");
		return FALSE;
	}

	/* Signal the shutdown event */
	DEBUG_PRINT("Terminating usbmuxd");
	if (FALSE == SetEvent(g_tContext.hShutdownEvent))
	{
		DEBUG_PRINT_WIN32_ERROR("SetEvent");
		return FALSE;
	}

	/* Wait for the main thread to finish */
	DWORD dwWaitResult = WaitForSingleObject(g_hMainThread, SHUTDOWN_TIMEOUT);
	switch (dwWaitResult)
	{
	/* The thread has been terminated */
	case WAIT_OBJECT_0:
		DEBUG_PRINT("usbmuxd thread was terminated");
		break;

	/* Timeout */
	case WAIT_TIMEOUT:
		DEBUG_PRINT("Timeout while waiting to usbmuxd's thread to terminate, killing the thread...");
		if (FALSE == TerminateThread(g_hMainThread, 0))
		{
			DEBUG_PRINT_WIN32_ERROR("TerminateThread");
		}
		break;

	/* Error */
	default:
		DEBUG_PRINT_WIN32_ERROR("WaitForSingleObject");
		return FALSE;
	}
	
	device_kill_connections();
	usb_shutdown();
	device_shutdown();
	client_shutdown();

	(void)WSACleanup();

	/* Cleanup */
	SAFE_CLOSE_HANDLE(g_hMainThread);
	SAFE_CLOSE_HANDLE(g_tContext.hReadyEvent);
	SAFE_CLOSE_HANDLE(g_tContext.hShutdownEvent);
	SecureZeroMemory((void *)&g_tContext, sizeof(g_tContext));

	return TRUE;
}

/******************************************************************************
 * usbmuxd_GetPort Function
 *****************************************************************************/
USBMUXD_API WORD usbmuxd_GetPort()
{
	return g_tContext.wClientsPort;
}

/******************************************************************************
 * usbmuxd_GetDevicesPort Function
 *****************************************************************************/
USBMUXD_API WORD usbmuxd_GetDevicesPort()
{
	return g_tContext.wDevicesPort;
}

/******************************************************************************
 * usbmuxd_AddDevice Function
 *****************************************************************************/
USBMUXD_API BOOL usbmuxd_AddDevice(LPCSTR pszPortName, HANDLE hCompletionEvent)
{
	VERIFY_USBMUXD_IS_RUNNING();

	/* Parse the port number, which is used as the device's location */
	uint32_t ulDeviceLocation = 0;
	sscanf_s(pszPortName, MCE_PORT_NAME_FORMAT, &ulDeviceLocation);

	if (usb_add_device(ulDeviceLocation, hCompletionEvent) < 0)
	{
		DEBUG_PRINT_WIN32_ERROR("usb_add_device");
		return FALSE;
	}

	return TRUE;
}

/******************************************************************************
 * usbmuxd_RemoveDevice Function
 *****************************************************************************/
USBMUXD_API BOOL usbmuxd_RemoveDevice(LPCSTR pszPortName, HANDLE hCompletionEvent)
{
	VERIFY_USBMUXD_IS_RUNNING();

	/* Parse the port number, which is used as the device's location */
	uint32_t ulDeviceLocation = 0;
	sscanf_s(pszPortName, MCE_PORT_NAME_FORMAT, &ulDeviceLocation);

	if (usb_remove_device(ulDeviceLocation, hCompletionEvent) < 0)
	{
		DEBUG_PRINT_WIN32_ERROR("usb_remove_device");
		return FALSE;
	}

	return TRUE;
}

/******************************************************************************
 * usbmuxd_StartMonitoringDevice Function
 *****************************************************************************/
USBMUXD_API BOOL usbmuxd_StartMonitoringDevice(LPCSTR pszPortName)
{
	return SetDeviceMonitoring(pszPortName, DEVICE_MONITOR_ALWAYS);
}

/******************************************************************************
 * usbmuxd_StoptMonitoringDevice Function
 *****************************************************************************/
USBMUXD_API BOOL usbmuxd_StoptMonitoringDevice(LPCSTR pszPortName)
{
	return SetDeviceMonitoring(pszPortName, DEVICE_MONITOR_DISABLE);
}

/******************************************************************************
 * usbmuxd_HasDevices Function
 *****************************************************************************/
USBMUXD_API BOOL usbmuxd_HasDevices()
{
	return (BOOL)usb_has_devices();
}

/******************************************************************************
 * Private Functions
 *****************************************************************************/
#ifdef USE_PORTDRIVER_SOCKETS
	/******************************************************************************
	 * CreateFdSets Function
	 *****************************************************************************/
	static int CreateFdSets(USBMUXD_SOCKETS * ptSockets, fd_set * pReadFds, fd_set * pWriteFds)
	{
		ATLASSERT(NULL != pReadFds);

		FD_ZERO(pReadFds);
		FD_ZERO(pWriteFds);

		/* Add the listening socket to the read fd set */
		int iFdCount = 1;
		FD_SET(ptSockets->hClientsListenSocket, pReadFds);

		/* Add the clients sockets */
		iFdCount += client_add_fds(pReadFds, pWriteFds);

		/* Add the devices sockets */
		iFdCount += usb_add_fds(pReadFds, pWriteFds);

		return iFdCount;
	}
#else
	/******************************************************************************
	 * CreateFdSets Function
	 *****************************************************************************/
	static int CreateFdSets(USBMUXD_SOCKETS * ptSockets, fd_set * pReadFds, fd_set * pWriteFds)
	{
		ATLASSERT(NULL != pReadFds);


		FD_ZERO(pReadFds);
		FD_ZERO(pWriteFds);

		/* Add the listening sockets to the read fd set */
		int iFdCount = 2;
		FD_SET(ptSockets->hClientsListenSocket, pReadFds);
		FD_SET(ptSockets->hDevicesListenSocket, pReadFds);

		/* Add the clients sockets */
		iFdCount += client_add_fds(pReadFds, pWriteFds);

		/* Add the devices sockets */
		iFdCount += device_add_fds(pReadFds, pWriteFds);

		return iFdCount;
	}

	/******************************************************************************
	 * CreateListenSockets Function
	 *****************************************************************************/
	static bool CreateListenSockets(USBMUXD_CONTEXT * ptContext, 
									USBMUXD_SOCKETS * ptSockets)
	{
		/* Create the client listening socket */
		if (FALSE == CreateListenSocket(&(ptSockets->hClientsListenSocket), &(ptContext->wClientsPort)))
		{
			DEBUG_PRINT_ERROR("CreateListenSocket has failed");
			return false;
		}
		SocketSetNonBlocking(ptSockets->hClientsListenSocket, TRUE);

		DEBUG_PRINT("usbmuxd is listening for clients on port %u", ptContext->wClientsPort);

		/* Create the device listening socket */
		if (FALSE == CreateListenSocket(&(ptSockets->hDevicesListenSocket), &(ptContext->wDevicesPort)))
		{
			DEBUG_PRINT_ERROR("CreateListenSocket has failed");

			SAFE_CLOSE_SOCKET(ptSockets->hClientsListenSocket);
			return false;
		}
		DEBUG_PRINT("usbmuxd is listening for devices on port %u", ptContext->wDevicesPort);

		return true;
	}	
#endif

/******************************************************************************
 * GetCurrentIterationTimeout Function
 *****************************************************************************/
static void GetCurrentIterationTimeout(timeval * ptSelectTimeout)
{
	int iTimeout = SOCKETS_SELECT_INTERVAL;
	int iDeviceTimeout = device_get_timeout();
	if (iDeviceTimeout < iTimeout)
	{
		iTimeout = iDeviceTimeout;
	}

	MS_TIMEOUT_TO_TIMEVAL(*ptSelectTimeout, iTimeout);
}

/******************************************************************************
 * MainThreadProc Function
 *****************************************************************************/
static DWORD WINAPI MainThreadProc(void * pvParam)
{
	



	USBMUXD_CONTEXT * ptContext = (USBMUXD_CONTEXT *)pvParam;
	USBMUXD_SOCKETS tSockets;

	/* Create the client listening socket */
	#ifdef USE_PORTDRIVER_SOCKETS
	if (FALSE == CreateListenSocket(&(tSockets.hClientsListenSocket), &(ptContext->wClientsPort)))
	#else
	if (false == CreateListenSockets(ptContext, &tSockets))
	#endif
	{
		DEBUG_PRINT_ERROR("CreateListenSocket has failed");
		EXIT_THREAD(0);
	}
	DEBUG_PRINT("usbmuxd is listening for clients on port %u", ptContext->wClientsPort);

	/* Set libusmuxd's port to our port (the preflight module uses libimobiledevice) */
	idevice_set_usbmuxd_port(ptContext->wClientsPort);
	
	/* Signal usbmuxd_Start we're ready */
	(void)SetEvent(ptContext->hReadyEvent);

	int iCurFdCount = 0;
	fd_set readFds;
	fd_set writeFds;

	/* In order to check the shutdown event while waiting for socket events,
	 * and allow the device layer to handle data timeouts, we'll run "select" 
	 * in intervals */
	timeval tSelectTimeout = { 0 };

	bool bShouldStop = false;
	int iRes = -1;
	while (true)
	{
		/* Check the shutdown event */
		DWORD dwWaitResult = WaitForSingleObject(ptContext->hShutdownEvent, 0);
		switch (dwWaitResult)
		{
		/* The thread has been terminated */
		case WAIT_OBJECT_0:
			DEBUG_PRINT("The shutdown event was signaled");
			bShouldStop = true;
			break;

		/* Timeout */
		case WAIT_TIMEOUT:
			break;

		/* Error */
		default:
			DEBUG_PRINT_WIN32_ERROR("WaitForSingleObject");
			bShouldStop = true;
		}

		if (bShouldStop)
		{
			break;
		}

		/* Wait for a "read" event on the listening and client sockets */
		GetCurrentIterationTimeout(&tSelectTimeout);
		iCurFdCount = CreateFdSets(&tSockets, &readFds, &writeFds);
		iRes = select(0, &readFds, &writeFds, NULL, &tSelectTimeout);
		if (SOCKET_ERROR == iRes)
		{
			DEBUG_PRINT_WSA_ERROR("select");
			break;
		}
		else if (0 == iRes)
		{
			/* select has timed out */
			usb_process(NULL);
			device_check_timeouts();
			continue;
		}

		/* Check for an event on the clients listening socket */
		if (FD_ISSET(tSockets.hClientsListenSocket, &readFds))
		{
			/* Make sure we have room for a new socket in our fdsets */
			BOOL bShouldRejectClient = FALSE;
			if ((iCurFdCount + 1) > FD_SETSIZE)
			{
				DEBUG_PRINT_ERROR("We have a new client, but we cannot handle more than %d sockets simultaneously", FD_SETSIZE);
				bShouldRejectClient = TRUE;
			}

			if (client_accept(tSockets.hClientsListenSocket, bShouldRejectClient) < 0)
			{
				DEBUG_PRINT_WSA_ERROR("accept");
			}
		}

		#ifdef USE_PORTDRIVER_SOCKETS
			/* Handle usb events */
			usb_process(&readFds);
		#else
			/* Check for an event on the devices listening socket */
			if (FD_ISSET(tSockets.hDevicesListenSocket, &readFds))
			{
				/* Make sure we have room for a new socket in our fdsets */
				BOOL bShouldRejectClient = FALSE;
				if ((iCurFdCount + 1) > FD_SETSIZE)
				{
					//LOG_ERROR("We have a device socket, but we cannot handle more than %d sockets simultaneously", FD_SETSIZE);
					bShouldRejectClient = TRUE;
				}

				if (device_accept_socket(tSockets.hDevicesListenSocket, bShouldRejectClient) < 0)
				{
					//LOG_WSA_ERROR("accept");
				}
			}

			/* Handle device socket events */
			device_process_sockets(&readFds, &writeFds);

			/* Handle usb events */
			usb_process(NULL);
		#endif
		
		/* Handle client socket events */
		client_process(&readFds, &writeFds);
	}

	//LOG_TRACE("usbmuxd thread is terminating");
	closesocket(tSockets.hClientsListenSocket);
	#ifndef USE_PORTDRIVER_SOCKETS
		closesocket(tSockets.hDevicesListenSocket);
	#endif
	
	EXIT_THREAD(0);
}

/******************************************************************************
 * SetDeviceMonitoring Function
 *****************************************************************************/
static BOOL SetDeviceMonitoring(LPCSTR pszPortName, enum device_monitor_state eMonitor)
{
	VERIFY_USBMUXD_IS_RUNNING();

	/* Parse the port number, which is used as the device's location */
	uint32_t ulDeviceLocation = 0;
	sscanf_s(pszPortName, MCE_PORT_NAME_FORMAT, &ulDeviceLocation);

	if (usb_set_device_monitoring(ulDeviceLocation, eMonitor, DEVICE_MONITORING_CHANGE_TIMEOUT) < 0)
	{
		//LOG_WIN32_ERROR("usb_set_device_monitoring");
		return FALSE;
	}

	return TRUE;
}