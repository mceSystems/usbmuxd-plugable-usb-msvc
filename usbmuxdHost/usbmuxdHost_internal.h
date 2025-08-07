/******************************************************************************
 * usbmuxdHost_internal.h
 *****************************************************************************/
#ifndef __MCE_USBMUXD_HOST_INTERNAL_H__
#define __MCE_USBMUXD_HOST_INTERNAL_H__

/******************************************************************************
 * Defs & Types
 *****************************************************************************/
/* Command line parsing */
#define CMD_ARGC (7)
#define CMD_ARG_PORT (1)
#define CMD_ARG_HUB_ADDRESS (2)
#define CMD_ARG_READY_EVENT (3)
#define CMD_ARG_SHUTDOWN_EVENT (4)
#define CMD_ARG_PLUGIN_PATH (5)
#define CMD_ARG_AUTOSHUTDOWN (6)




#define SHUTDOWN_EVENT_WAIT_INTERVAL (2500)
#define NO_DEVICES_SHUTDOWN_TIMEOUT (5000)

#define WAIT_EVENTS_COUNT (2)
#define WAIT_EVENT_SHUTDOWN (0)
#define WAIT_EVENT_KEEP_ALIVE (1)

/******************************************************************************
 * Internal Functions Declarations
 *****************************************************************************/
static int RunUsbmuxdHost(WORD wPort, DWORD dwHubAddress, LPCWSTR pszReadyEventName, LPCWSTR pszShutdownEventName, LPCWSTR pLuginPath,  LPCWSTR pAutoShutdown);
static int InternalWinMain();

#endif /* __MCE_USBMUXD_HOST_INTERNAL_H__ */