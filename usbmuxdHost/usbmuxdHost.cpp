/******************************************************************************
 * usbmuxdHost.cpp
 *****************************************************************************/
/******************************************************************************
 * Includes
 *****************************************************************************/
#include "stdafx.h"
#include "..\usbmuxd.h"
#include "usbmuxdHost_internal.h"

/* MCE includes */
#include "DebugPrint.h"
#include "WindowsUtil.h"


#define USBMUXD_HOST_MUTEX_NAME_FORMAT ("Global\\MCE_{15b31527-2034-4115-8036-e89ef3f4ee0d}_%u")
#define USBMUXD_HOST_SHUTDOWN_EVENT_NAME_FORMAT ("Global\\MCE_{49facbfc-b65c-4bc5-85b0-7ac10307ec5f}_%u")
#define USBMUXD_HOST_KEEPALIVE_EVENT_NAME_FORMAT ("Global\\MCE_{7fde2786-5bb0-4f3e-8e32-570244010db1}_%u")








/******************************************************************************
 * RunUsbmuxdHost Function
 *****************************************************************************/
static int RunUsbmuxdHost(WORD wPort, DWORD dwHubAddress, LPCWSTR pszReadyEventName, LPCWSTR pszShutdownEventName, LPCWSTR pPluginPath, LPCWSTR pAutoShutdown)
{
	int iRet = EXIT_FAILURE;
	//MessageBoxA(NULL, "RunUsbmuxdHost", "RunUsbmuxdHost", MB_OK);
	OutputDebugStringA("UMDR 1 ");
	CString autoshutdownSTR(pAutoShutdown);
	BOOL bAutoShutdown = autoshutdownSTR.CompareNoCase("AutoShutdownTRUE") == 0;
	CString strHostMutexName;
	CString strKeepAliveEventName;
	HANDLE hHostMutex = NULL;
	HANDLE hKeepAliveEvent = NULL;
	DWORD dwWaitResult = WAIT_FAILED;
	BOOL bShouldShutdown = FALSE;
	DWORD dwNoDevicesTimestamp = 0;
	HANDLE ahWaitEvents[WAIT_EVENTS_COUNT] = { NULL };

	CStringA readyName; 
	readyName.AppendFormat("UMDR Ready Event %ws ", pszReadyEventName);

	OutputDebugStringA(readyName.GetBuffer());
	HANDLE hReadEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, pszReadyEventName);
	if (!hReadEvent) {
		hReadEvent = CreateEventW(
			/* lpEventAttributes */ NULL,
			/* bManualReset */      FALSE,
			/* bInitialState */     FALSE,
			/* lpName */            pszReadyEventName
			);

		if (!hReadEvent) {
			// handle error, e.g. log GetLastError()
		}
	}
	HANDLE hShutdownEvent = OpenEventW(SYNCHRONIZE, FALSE, pszShutdownEventName);
	if (!hShutdownEvent) {
		hShutdownEvent = CreateEventW(
			/* lpEventAttributes */ NULL,
			/* bManualReset */      FALSE,
			/* bInitialState */     FALSE,
			/* lpName */            pszShutdownEventName
			);

		if (!hShutdownEvent) {
			// handle error, e.g. log GetLastError()
		}
	}

	if ((FALSE == IS_VALID_HANDLE(hReadEvent)) || (FALSE == IS_VALID_HANDLE(hShutdownEvent)))
	{
		DEBUG_PRINT_WIN32_ERROR("OpenEvent");
		goto lblCleanup;
	}

	/* Create our "single instance" mutex, and verify we are the onky usbmuxdHost
	 * process */
	strHostMutexName.Format(USBMUXD_HOST_MUTEX_NAME_FORMAT, wPort);
	strKeepAliveEventName.Format(USBMUXD_HOST_KEEPALIVE_EVENT_NAME_FORMAT, wPort);
	hHostMutex = CreateMutexAllAccess(strHostMutexName, TRUE);
	if (FALSE == IS_VALID_HANDLE(hHostMutex))
	{
		DEBUG_PRINT_WIN32_ERROR("CreateMutexAllAccess");
		goto lblCleanup;
	}
	OutputDebugStringA("UMDR 2 ");
	/* Check if another instance is running */
	if (ERROR_ALREADY_EXISTS == GetLastError())
	{
		DEBUG_PRINT("Another instance is already running");

		/* Signal the keep alive event, to reset the other usbmuxdHost's
		 * shutdown timer */
		hKeepAliveEvent = OpenEvent(EVENT_MODIFY_STATE, FALSE, strKeepAliveEventName);
		if (IS_VALID_HANDLE(hKeepAliveEvent))
		{
			if (FALSE == SetEvent(hKeepAliveEvent))
			{
				DEBUG_PRINT_WIN32_ERROR("SetEvent");
			}
			SAFE_CLOSE_HANDLE(hKeepAliveEvent);
		}
		else
		{
			DEBUG_PRINT_WIN32_ERROR("OpenEvent");
		}

		/* Another instance is running. We'll still signal 
		 * the ready event, because its a valid state (and the caller
		 * can start working with the other intance). */
		if (SetEvent(hReadEvent))
		{
			iRet = EXIT_SUCCESS;
		}
		else
		{
			DEBUG_PRINT_WIN32_ERROR("SetEvent");
		}

		goto lblCleanup;
	}
	OutputDebugStringA("UMDR 3 ");
	/* Create the keep alive event */
	hKeepAliveEvent = CreateEventAllAccess(strKeepAliveEventName);
	if (FALSE == IS_VALID_HANDLE(hKeepAliveEvent))
	{
		DEBUG_PRINT_WIN32_ERROR("CreateEventAllAccess");
		goto lblCleanup;
	}
	OutputDebugStringA("UMDR 4 ");
	//MessageBoxA(NULL, "WTF", "WTF", MB_OK);
	/* Start usbmuxd */
	if (FALSE == usbmuxd_Start(wPort, dwHubAddress, pPluginPath))
	{
		DEBUG_PRINT_ERROR("UMDR Failed to start usbmuxd");
		goto lblCleanup;
	}
	OutputDebugStringA("UMDR 5 ");
	/* Signal the usbmuxdHostClient we are ready */
	if (FALSE == SetEvent(hReadEvent))
	{
		DEBUG_PRINT_WIN32_ERROR("SetEvent");
	}

	/* Wait for the shutdown and keep alive signals */
	ahWaitEvents[WAIT_EVENT_SHUTDOWN] = hShutdownEvent;
	ahWaitEvents[WAIT_EVENT_KEEP_ALIVE] = hKeepAliveEvent;
	while (FALSE == bShouldShutdown)
	{                                                                
		dwWaitResult = WaitForMultipleObjects(WAIT_EVENTS_COUNT, ahWaitEvents, FALSE, SHUTDOWN_EVENT_WAIT_INTERVAL);
		switch (dwWaitResult)
		{
		/* Shutdown event */
		case WAIT_OBJECT_0 + WAIT_EVENT_SHUTDOWN:
			DEBUG_MCE("Shutdown event was signaled");
			bShouldShutdown = TRUE;
			break;
		
		/* Keepalive event */
		case WAIT_OBJECT_0 + WAIT_EVENT_KEEP_ALIVE:
			/* Reset the timeout base time */
			DEBUG_PRINT("Keepalive event was signaled");
			dwNoDevicesTimestamp = 0;
			break;

		/* Timeout (which is normal, since we are waiting in intervals) */
		case WAIT_TIMEOUT:
			/* Check if we have monitored devices. When we don't have any devices
			 * left, after a certain timeout - we'll shutdown */
			if (!bAutoShutdown || usbmuxd_HasDevices())
			{
				/* Reset the timeout base time */
				dwNoDevicesTimestamp = 0;
			}
			else
			{
				/* If this is the first time we don't have a device - store the timestamp */
				if (0 == dwNoDevicesTimestamp)
				{
					dwNoDevicesTimestamp = GetTickCount();
				}
				/* If its not the first time - check if the timeout has expired */
				else
				{
					if ((GetTickCount() - dwNoDevicesTimestamp) >= NO_DEVICES_SHUTDOWN_TIMEOUT)
					{
						DEBUG_MCE("Shutting down because we don't have a device");
						bShouldShutdown = TRUE;
					}
				}
			}

			break;

		/* Error */
		default:
			DEBUG_PRINT_WIN32_ERROR("WaitForSingleObject");
			bShouldShutdown = TRUE;
		}
	}

	/* Shutdown usbmuxd */
	DEBUG_PRINT("Calling usbmuxd_Shutdown");
	if (usbmuxd_Shutdown())
	{
		iRet = EXIT_SUCCESS;
	}
	else
	{
		DEBUG_PRINT_WIN32_ERROR("usbmuxd_Shutdown");
	}

lblCleanup:
	SAFE_CLOSE_HANDLE(hReadEvent);
	SAFE_CLOSE_HANDLE(hShutdownEvent);
	SAFE_CLOSE_HANDLE(hHostMutex);
	SAFE_CLOSE_HANDLE(hKeepAliveEvent);

	return iRet;
}

/******************************************************************************
 * InternalWinMain Function
 *****************************************************************************/



static int InternalWinMain()
{
	//MessageBoxA(NULL, "InternalWinMain", "InternalWinMain", MB_OK);
	int iRet = EXIT_FAILURE;

	/* Parse the command line */
	int argc = 0;
	LPWSTR * argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (NULL == argv)
	{
		DEBUG_PRINT_WIN32_ERROR("CommandLineToArgvW");
		return iRet;
	}
	if (CMD_ARGC != argc)
	{
		DEBUG_PRINT_ERROR("Invalid command line");
		goto lblCleanup;
	}

	/* Run the usbmuxd */
	iRet = RunUsbmuxdHost((WORD)_wtoi(argv[CMD_ARG_PORT]), 
						  wcstoul(argv[CMD_ARG_HUB_ADDRESS], NULL, 10),
						  argv[CMD_ARG_READY_EVENT], 
						  argv[CMD_ARG_SHUTDOWN_EVENT],
						  argv[CMD_ARG_PLUGIN_PATH],
						argv[CMD_ARG_AUTOSHUTDOWN]

		);

lblCleanup:
	(void)LocalFree(argv);
	return iRet;
}

/******************************************************************************
 * WinMain Function
 *****************************************************************************/
int CALLBACK WinMain(HINSTANCE	hInstance,
					 HINSTANCE	hPrevInstance,
					 LPSTR		lpCmdLine,
					 int		nCmdShow)
{
	__try
	{
		//MessageBoxA(NULL, "WinMain", "usbmuxdHost", MB_OK);
		return InternalWinMain();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		__try
		{
			DEBUG_PRINT_ERROR("UMDR An unhandleded exception was caught: 0x%x", GetExceptionCode());
		}
		__except (EXCEPTION_EXECUTE_HANDLER) 
		{
		}
	}

	return EXIT_FAILURE;
}
