#pragma once
#include "usbmuxd_com_plugin_api.h"
#include "debugprint.h"

char* g_pPluginPath = ""; 
HMODULE g_hPlugin = NULL;

typedef int(*com_plugin_init_type) (DWORD hubAddress);
typedef int(*com_plugin_deinit_type) ();
typedef int(*com_plugin_unregister_com_notification_type) (HANDLE hCookie); 
typedef int(*com_plugin_register_com_notification_type) (PFN_ComPluginChange pfnPortChange, LPVOID pContext, LPHANDLE phCookie);
typedef int(*com_plugin_transfer_type) (COMHANDLE* pHandle, BOOL bRead, BYTE bEndPoint, LPVOID lpBuffer, DWORD dwBytesToTransfer, LPDWORD lpdwBytesTransferred, LPVOID lpSetupPacket, DWORD dwTimeout, LPOVERLAPPED lpOverlapped);
typedef int(*com_plugin_get_transfer_result_type) (COMHANDLE* pHandle, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, BOOL bWait);
typedef int(*com_plugin_close_type) (COMHANDLE* pHandle);
typedef int(*com_plugin_open_by_name_type) (const char * szPortName, COMHANDLE* pOutHandle);
typedef int(*com_pluging_get_device_descriptor_type) (COMHANDLE* pHandle, BYTE * pDeviceDescriptor);
typedef int(*com_plugin_get_ascii_string_descriptor_type) (COMHANDLE * pHandle, BYTE nStrIndex, CHAR * pszBuffer, DWORD dwBufferSize);
typedef int(*com_plugin_get_configuration_descriptor_type) (COMHANDLE* pHandle, BYTE bConfiguration, BYTE * pConfiguration, PDWORD pdwConfigurationSize);
typedef int(*com_plugin_select_configuration_type) (COMHANDLE* pHandle, DWORD dwConfiguration);
typedef int(*com_plugin_log_type)(LPCSTR pLog);
com_plugin_init_type cp_init = NULL;
com_plugin_deinit_type cp_deinit = NULL;
com_plugin_unregister_com_notification_type cp_unregister_com_notification = NULL;
com_plugin_register_com_notification_type cp_register_com_notification = NULL;
com_plugin_transfer_type cp_transfer = NULL;
com_plugin_get_transfer_result_type cp_get_transfer_result = NULL;
com_plugin_close_type cp_close = NULL;
com_plugin_open_by_name_type cp_open_by_name = NULL;
com_pluging_get_device_descriptor_type cpg_get_device_descriptor = NULL;
com_plugin_get_ascii_string_descriptor_type cp_get_ascii_string_descriptor = NULL;
com_plugin_get_configuration_descriptor_type cp_get_configuration_descriptor = NULL;
com_plugin_select_configuration_type cp_select_configuration = NULL;
com_plugin_log_type cp_log = NULL;



int com_plugin_client_init(LPCWSTR pCommPluginPath)
{
	int ret = COM_ERR_FATAL;

	g_hPlugin = LoadLibraryW(pCommPluginPath);
	int le = GetLastError(); 
	if (g_hPlugin !=NULL && g_hPlugin != INVALID_HANDLE_VALUE)
	{
		cp_init = (com_plugin_init_type)GetProcAddress(g_hPlugin, "com_plugin_init");
		cp_deinit = (com_plugin_deinit_type)GetProcAddress(g_hPlugin,"com_plugin_deinit");
		cp_unregister_com_notification = (com_plugin_unregister_com_notification_type)GetProcAddress(g_hPlugin, "com_plugin_unregister_com_notification");
		cp_register_com_notification = (com_plugin_register_com_notification_type)GetProcAddress(g_hPlugin, "com_plugin_register_com_notification");
		cp_transfer = (com_plugin_transfer_type)GetProcAddress(g_hPlugin, "com_plugin_transfer");
		cp_get_transfer_result = (com_plugin_get_transfer_result_type)GetProcAddress(g_hPlugin, "com_plugin_get_transfer_result");
		cp_close = (com_plugin_close_type)GetProcAddress(g_hPlugin, "com_plugin_close");
		cp_open_by_name = (com_plugin_open_by_name_type)GetProcAddress(g_hPlugin, "com_plugin_open_by_name");
		cpg_get_device_descriptor = (com_pluging_get_device_descriptor_type)GetProcAddress(g_hPlugin, "com_pluging_get_device_descriptor");
		cp_get_ascii_string_descriptor = (com_plugin_get_ascii_string_descriptor_type)GetProcAddress(g_hPlugin, "com_plugin_get_ascii_string_descriptor");
		cp_get_configuration_descriptor = (com_plugin_get_configuration_descriptor_type)GetProcAddress(g_hPlugin, "com_plugin_get_configuration_descriptor");
		cp_select_configuration = (com_plugin_select_configuration_type)GetProcAddress(g_hPlugin, "com_plugin_select_configuration");
		cp_log = (com_plugin_log_type)GetProcAddress(g_hPlugin, "com_plugin_log");
		ret = COM_OK; 
	}

	//load plugin library 
	// load procs
	return ret;

}

int com_plugin_init(uint32_t hub_address)
{
	int ret =  COM_ERR_FATAL;
	if (cp_init)
	{
		ret = cp_init(hub_address);
	}
	return ret; 
}


int  com_plugin_deinit()
{
	int ret = COM_ERR_FATAL;
	if (cp_deinit)
	{
		ret = cp_deinit();
	}
	return ret;
}
int	 com_plugin_unregister_com_notification(HANDLE hCookie)
{
	int ret = COM_ERR_FATAL;
	if (cp_unregister_com_notification)
	{
		ret = cp_unregister_com_notification(hCookie);
	}
	return ret;
}
int  com_plugin_register_com_notification(PFN_ComPluginChange pfnPortChange, LPVOID pContext, LPHANDLE phCookie)
{
	int ret = COM_ERR_FATAL;
	if (cp_register_com_notification)
	{
		ret = cp_register_com_notification(pfnPortChange,  pContext,  phCookie);
	}
	return ret;
}
int  com_plugin_transfer(COMHANDLE* pHandle, BOOL bRead, BYTE bEndPoint, LPVOID lpBuffer, DWORD dwBytesToTransfer, LPDWORD lpdwBytesTransferred, LPVOID lpSetupPacket, DWORD dwTimeout, LPOVERLAPPED lpOverlapped)
{
	int ret = COM_ERR_FATAL;
	if (cp_transfer)
	{
		ret = cp_transfer(pHandle,  bRead,  bEndPoint,  lpBuffer,  dwBytesToTransfer,  lpdwBytesTransferred,  lpSetupPacket,  dwTimeout,  lpOverlapped);
	}
	return ret;
}
int  com_plugin_get_transfer_result(COMHANDLE* pHandle, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, BOOL bWait)
{
	int ret = COM_ERR_FATAL;
	if (cp_get_transfer_result)
	{
		ret = cp_get_transfer_result(pHandle,  lpOverlapped,  lpNumberOfBytesTransferred,  bWait);
	}
	return ret;
}
int  com_plugin_close(COMHANDLE* pHandle)
{
	int ret = COM_ERR_FATAL;
	if (cp_close)
	{
		ret = cp_close(pHandle);
	}
	return ret;
}
int  com_plugin_open_by_name(const char * szPortName, COMHANDLE* pOutHandle)
{
	int ret = COM_ERR_FATAL;
	if (cp_open_by_name)
	{
		ret = cp_open_by_name(szPortName,  pOutHandle);
	}
	return ret;
}
int  com_pluging_get_device_descriptor(COMHANDLE* pHandle, BYTE * pDeviceDescriptor)
{
	int ret = COM_ERR_FATAL;
	if (cpg_get_device_descriptor)
	{
		ret = cpg_get_device_descriptor(pHandle,  pDeviceDescriptor);
	}
	return ret;
}
int  com_plugin_get_ascii_string_descriptor(COMHANDLE * pHandle, BYTE nStrIndex, CHAR * pszBuffer, DWORD dwBufferSize)
{
	int ret = COM_ERR_FATAL;
	if (cp_get_ascii_string_descriptor)
	{
		ret = cp_get_ascii_string_descriptor(pHandle,  nStrIndex,  pszBuffer,  dwBufferSize);
	}
	return ret;
}
int  com_plugin_get_configuration_descriptor(COMHANDLE* pHandle, BYTE bConfiguration, BYTE * pConfiguration, PDWORD pdwConfigurationSize)
{
	int ret = COM_ERR_FATAL;
	if (cp_get_configuration_descriptor)
	{
		ret = cp_get_configuration_descriptor(pHandle,  bConfiguration, pConfiguration,  pdwConfigurationSize);
	}
	return ret;
}
int  com_plugin_select_configuration(COMHANDLE* pHandle, DWORD dwConfiguration)
{
	int ret = COM_ERR_FATAL;
	if (cp_select_configuration)
	{
		ret = cp_select_configuration(pHandle,  dwConfiguration);
	}
	return ret;
}