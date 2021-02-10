/******************************************************************************
 * usbmuxdHostIPC.h
 *****************************************************************************/
#ifndef __MCE_USBMUXD_HOST_IPC_H__
#define __MCE_USBMUXD_HOST_IPC_H__

typedef enum _USBMUXD_HOST_STATUS
{
	USBMUXD_HOST_STATUS_SUCCESS,
	USBMUXD_HOST_STATUS_INIT_FAILED,
	USBMUXD_HOST_STATUS_ERROR
} USBMUXD_HOST_STATUS;

typedef enum _USBMUXD_HOST_COMMAND
{
	USBMUXD_HOST_COMMAND_INIT,
	USBMUXD_HOST_COMMAND_ADD_DEVICE,
	USBMUXD_HOST_COMMAND_REMOVE_DEVICE,
	USBMUXD_HOST_COMMAND_SHUTDOWN
} USBMUXD_HOST_COMMAND;

#pragma pack(1)
typedef struct _USBMUXD_HOST_RESPONSE
{
	USBMUXD_HOST_COMMAND eCommand;
	USBMUXD_HOST_STATUS eStatus;
} USBMUXD_HOST_RESPONSE;

/******************************************************************************
 * Requests Bodies
 *****************************************************************************/
#define USBMUXD_MAX_PORT_NAME_SIZE (16)

#pragma pack(1)
typedef struct _USBMUXD_ADD_DEVICE_REQUEST
{
	CHAR szPortName[USBMUXD_MAX_PORT_NAME_SIZE + 1];
	DWORD dwTimeout;
	BOOL bAutoMonitor;
} USBMUXD_ADD_DEVICE_REQUEST;

#pragma pack(1)
typedef struct _USBMUXD_REMOVE_DEVICE_REQUEST
{
	CHAR szPortName[USBMUXD_MAX_PORT_NAME_SIZE + 1];
	DWORD dwTimeout;
} USBMUXD_REMOVE_DEVICE_REQUEST;

#endif /* __MCE_USBMUXD_HOST_IPC_H__ */