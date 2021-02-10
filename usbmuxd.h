/******************************************************************************
 * usbmuxd.h
 *****************************************************************************/
#ifndef __MCE_USBMUXD_H__
#define __MCE_USBMUXD_H__

/******************************************************************************
 * Includes
 *****************************************************************************/


/******************************************************************************
 * Defs & Types
 *****************************************************************************/
/* Dll symbols linkage settings */
#ifdef USBMUXD_EXPORTS
	#define USBMUXD_API __declspec(dllexport) 
#else
	#define USBMUXD_API __declspec(dllimport) 
#endif

#define USBMUXD_DEDAULT_PORT (37015)

/******************************************************************************
 * usbmuxd_Start Function
 *****************************************************************************/
USBMUXD_API BOOL usbmuxd_Start(WORD wPort, DWORD dwHubAddress , LPCWSTR pPluginPath);

/******************************************************************************
 * usbmuxd_Shutdown Function
 *****************************************************************************/
USBMUXD_API BOOL usbmuxd_Shutdown();

/******************************************************************************
 * usbmuxd_GetPort Function
 *****************************************************************************/
USBMUXD_API WORD usbmuxd_GetPort();

/******************************************************************************
 * usbmuxd_GetDevicesPort Function
 *****************************************************************************/
USBMUXD_API WORD usbmuxd_GetDevicesPort();

/******************************************************************************
 * usbmuxd_AddDevice Function
 *****************************************************************************/
USBMUXD_API BOOL usbmuxd_AddDevice(LPCSTR pszPortName, HANDLE hCompletionEvent);

/******************************************************************************
 * usbmuxd_RemoveDevice Function
 *****************************************************************************/
USBMUXD_API BOOL usbmuxd_RemoveDevice(LPCSTR pszPortName, HANDLE hCompletionEvent);

/******************************************************************************
 * usbmuxd_StartMonitoringDevice Function
 *****************************************************************************/
USBMUXD_API BOOL usbmuxd_StartMonitoringDevice(LPCSTR pszPortName);

/******************************************************************************
 * usbmuxd_StoptMonitoringDevice Function
 *****************************************************************************/
USBMUXD_API BOOL usbmuxd_StoptMonitoringDevice(LPCSTR pszPortName);

/******************************************************************************
 * usbmuxd_HasDevices Function
 *****************************************************************************/
USBMUXD_API BOOL usbmuxd_HasDevices();

#endif /* __MCE_USBMUXD_H__ */