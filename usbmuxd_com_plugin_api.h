#pragma once 

#define COM_OK 0
#define COM_ERR_FATAL -1


#define COM_MCEUSB 7

typedef struct COMHANDLE{
	HANDLE hContext;
	unsigned int dwPortID; 
	WORD wVID;
	WORD wPID;
	bool bTurbo;//		char pRes[MAX_PATH] = { 0 };GetPrivateProfileStringA("Main", "DisableMuxdTurbo", "FALSE", pRes, MAX_PATH, mceWorkingFolder.GetBuffer());
	BYTE reserved[128]; 

};
typedef void(*PFN_ComPluginChange)(LPVOID pContext, const DWORD dwEvent, const char * szComtName);

int  com_plugin_init(DWORD hubAddress);
int  com_plugin_deinit();
int	 com_plugin_unregister_com_notification(HANDLE hCookie);
int  com_plugin_register_com_notification(PFN_ComPluginChange pfnPortChange, LPVOID pContext, LPHANDLE phCookie);
int  com_plugin_transfer(COMHANDLE* pHandle, BOOL bRead, BYTE bEndPoint, LPVOID lpBuffer, DWORD dwBytesToTransfer, LPDWORD lpdwBytesTransferred, LPVOID lpSetupPacket, DWORD dwTimeout, LPOVERLAPPED lpOverlapped); 
int  com_plugin_get_transfer_result(COMHANDLE* pHandle, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, BOOL bWait);
int  com_plugin_close(COMHANDLE* pHandle);
int  com_plugin_open_by_name(const char * szPortName, COMHANDLE* pOutHandle);
int  com_pluging_get_device_descriptor(COMHANDLE* pHandle, BYTE * pDeviceDescriptor);
int  com_plugin_get_ascii_string_descriptor(COMHANDLE * pHandle, BYTE nStrIndex, CHAR * pszBuffer, DWORD dwBufferSize);
int  com_plugin_get_configuration_descriptor(COMHANDLE* pHandle, BYTE bConfiguration, BYTE * pConfiguration, PDWORD pdwConfigurationSize);
int  com_plugin_select_configuration(COMHANDLE* pHandle, DWORD dwConfiguration);