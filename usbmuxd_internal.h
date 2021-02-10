/******************************************************************************
 * usbmuxd_internal.h
 *****************************************************************************/
#ifndef __MCE_USBMUXD_INTERNAL_H__
#define __MCE_USBMUXD_INTERNAL_H__

/******************************************************************************
 * Defs & Types
 *****************************************************************************/
/* All timeouts are in milliseconds */
#define SOCKETS_SELECT_INTERVAL (200)
#define INIT_TIMEOUT (2000)
#define SHUTDOWN_TIMEOUT (5000)
#define DEVICE_MONITORING_CHANGE_TIMEOUT (3000)

#define IS_USBMUXD_RUNNING() (IS_VALID_HANDLE(g_hMainThread))

#define VERIFY_USBMUXD_IS_RUNNING() if (FALSE == IS_USBMUXD_RUNNING())\
									{\
										return FALSE; \
									}

#define MCE_PORT_NAME_FORMAT ("MCE%u")

typedef struct _USBMUXD_CONTEXT
{
	HANDLE hReadyEvent;
	HANDLE hShutdownEvent;
	WORD wClientsPort;
	WORD wDevicesPort;
} USBMUXD_CONTEXT;

typedef struct _USBMUXD_SOCKETS
{
	SOCKET hClientsListenSocket;
	#ifndef USE_PORTDRIVER_SOCKETS
		SOCKET hDevicesListenSocket;
	#endif
	
	CAtlList<SOCKET> devicesSockets;
} USBMUXD_SOCKETS;

/******************************************************************************
 * Globals
 *****************************************************************************/
static HANDLE g_hMainThread = NULL;
static USBMUXD_CONTEXT g_tContext = { 0 };

/******************************************************************************
 * Internal Functions Declarations
 *****************************************************************************/
/******************************************************************************
 * CreateFdSets Function
 *****************************************************************************/
static int CreateFdSets(fd_set * pReadFds, SOCKET hListenSocket, const CAtlList<SOCKET>& clientSockets);

/******************************************************************************
 * GetCurrentIterationTimeout Function
 *****************************************************************************/
static void GetCurrentIterationTimeout(timeval * ptSelectTimeout);

/******************************************************************************
 * MainThreadProc Function
 *****************************************************************************/
static DWORD WINAPI MainThreadProc(void * pvParam);

/******************************************************************************
 * CreateListenSockets Function
 *****************************************************************************/
static bool CreateListenSockets(USBMUXD_CONTEXT * ptContext, 
								USBMUXD_SOCKETS * ptSockets);

/******************************************************************************
 * SetDeviceMonitoring Function
 *****************************************************************************/
static BOOL SetDeviceMonitoring(LPCSTR pszPortName, enum device_monitor_state eMonitor);

#endif /* __MCE_USBMUXD_INTERNAL_H__ */