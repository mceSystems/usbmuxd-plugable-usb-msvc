/******************************************************************************
 * WindowsUtil.cpp
 *****************************************************************************/
/******************************************************************************
 * Includes
 *****************************************************************************/
#include "stdafx.h"

/* Include the needed headers (and not rely on stdafx.h) just for the intellisense */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <atlstr.h>

#include "WindowsUtil.h"
#include "DebugPrint.h"

#pragma comment (lib, "Rpcrt4.lib")

/******************************************************************************
 * Defs
 *****************************************************************************/
#define GLOBAL_UNIQUE_NAME_FORMAT ("Global\\MCE_OBJECT_{%s}")
#define LOCAL_UNIQUE_NAME_FORMAT ("MCE_OBJECT_{%s}")

/******************************************************************************
 * Exported Functions Definitions
 *****************************************************************************/
/******************************************************************************
 * CreateNullSecurityAttributes Function
 *****************************************************************************/

BOOL CreateNullSecurityAttributes(OUT LPSECURITY_ATTRIBUTES ptSecurityAttributes)
{
	if (NULL == ptSecurityAttributes)
	{
		return FALSE;
	}

	/* Allocate and init a SECURITY_DESCRIPTOR struct. 
	 * Note: LocalAlloc is used because accroding to MSDN, this struct she should 
	 * be properly aligned (which is guaranteed by LocalAlloc and malloc)/ */
	PSECURITY_DESCRIPTOR ptNullSecurityDesc = (PSECURITY_DESCRIPTOR)LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
	if (NULL == ptNullSecurityDesc)
	{
		goto lblCleanup;
	}
	if (FALSE == InitializeSecurityDescriptor(ptNullSecurityDesc, SECURITY_DESCRIPTOR_REVISION))
	{
		goto lblCleanup;
	}

	if (FALSE == SetSecurityDescriptorDacl(ptNullSecurityDesc, TRUE, NULL, FALSE))
	{
		goto lblCleanup;
	}

	ptSecurityAttributes->nLength = sizeof(SECURITY_ATTRIBUTES); 
	ptSecurityAttributes->lpSecurityDescriptor = ptNullSecurityDesc;
	ptSecurityAttributes->bInheritHandle = FALSE;

	return TRUE;

lblCleanup:
	if (NULL != ptNullSecurityDesc)
	{
		LocalFree(ptNullSecurityDesc);
	}

	return FALSE;
}

/******************************************************************************
 * FreeNullSecurityAttributes Function
 *****************************************************************************/
void FreeNullSecurityAttributes(LPSECURITY_ATTRIBUTES ptSecurityAttributes)
{
	if (NULL == ptSecurityAttributes)
	{
		return;
	}

	if (NULL != ptSecurityAttributes->lpSecurityDescriptor)
	{
		LocalFree(ptSecurityAttributes->lpSecurityDescriptor);
		ptSecurityAttributes->lpSecurityDescriptor = NULL;
	}
}

/******************************************************************************
 * CreateUuidStringWithFormat Function
 * Internal helper function for CreateUuidString and CreateUniqueObjectName.
 *****************************************************************************/
static BOOL CreateUuidStringWithFormat(LPCSTR pszFormat, OUT CStringA& strUuid)
{
	/* Create a new UUID */
	UUID tNewUuid = { 0 };
	RPC_STATUS lResult = UuidCreate(&tNewUuid);
	if ((RPC_S_OK != lResult) && (RPC_S_UUID_LOCAL_ONLY != lResult))
	{
		return FALSE;
	}

	/* Convert the UUID into string */
	UCHAR * pszUuid = NULL;
	if (RPC_S_OK != UuidToStringA(&tNewUuid, &pszUuid))
	{
		return FALSE;
	}

	if (pszFormat)
	{
		strUuid.Format(pszFormat, pszUuid);
	}
	else
	{
		strUuid = pszUuid;
	}

	RpcStringFreeA(&pszUuid);
	return TRUE;
}

/******************************************************************************
 * CreateUuidString Function
 *****************************************************************************/
BOOL CreateUuidString(OUT CStringA& strUuid)
{
	return CreateUuidStringWithFormat(NULL, strUuid);
}

/******************************************************************************
 * CreateUniqueObjectName Function
 *****************************************************************************/
CStringA CreateUniqueObjectName(BOOL bCreateInGlobal)
{
	CStringA strUniqueName;

	if (bCreateInGlobal)
	{
		CreateUuidStringWithFormat(GLOBAL_UNIQUE_NAME_FORMAT, strUniqueName);
	}
	else
	{
		CreateUuidStringWithFormat(LOCAL_UNIQUE_NAME_FORMAT, strUniqueName);
	}

	return strUniqueName;
}

/******************************************************************************
 * CreateSharedMemory Function
 *****************************************************************************/
BOOL CreateSharedMemory(const CStringA& strName, DWORD dwSize, OUT MCE_SHARED_MEMORY * ptSharedMem)
{
	if (NULL == ptSharedMem)
	{
		return FALSE;
	}

	/* Create an "all access" Security Attributes for the file mapping */
	SECURITY_ATTRIBUTES tSecurityAttributes = {0};
	if (FALSE == CreateNullSecurityAttributes(&tSecurityAttributes))
	{
		return FALSE;
	}

	/* Create a file mapping, backed by the page file */
	HANDLE hFileMapping = CreateFileMappingA(INVALID_HANDLE_VALUE,
											 NULL,      
											 PAGE_READWRITE,
											 0,
											 dwSize,
											 strName);
	FreeNullSecurityAttributes(&tSecurityAttributes);
	if (FALSE == IS_VALID_HANDLE(hFileMapping))
	{
		return FALSE;
	}

	/* Map the shared memory to our address space */
	void * pvMapped =  MapViewOfFile(hFileMapping, FILE_MAP_ALL_ACCESS, 0, 0, dwSize);
	if (FALSE == pvMapped)
	{
		SAFE_CLOSE_HANDLE(hFileMapping);
		return FALSE;
	}

	ptSharedMem->hFileMapping = hFileMapping;
	ptSharedMem->pvMappedBuffer = pvMapped;
	ptSharedMem->dwSize = dwSize;

	return TRUE;
}

/******************************************************************************
 * CloseSharedMemory Function
 *****************************************************************************/
BOOL CloseSharedMemory(MCE_SHARED_MEMORY * ptSharedMem)
{
	if (NULL == ptSharedMem)
	{
		return FALSE;
	}

	if (NULL != ptSharedMem->pvMappedBuffer)
	{
		if (FALSE == UnmapViewOfFile(ptSharedMem->pvMappedBuffer))
		{
			return FALSE;
		}
	}

	SAFE_CLOSE_HANDLE(ptSharedMem->hFileMapping);

	return TRUE;
}

/******************************************************************************
 * CreateMutexAllAccess Function
 *****************************************************************************/
HANDLE CreateMutexAllAccess(const CStringA& strName, BOOL bInitialOwner)
{
	/* Create an "all access" Security Attributes for the mutex */
	SECURITY_ATTRIBUTES tSecurityAttributes = {0};
	if (FALSE == CreateNullSecurityAttributes(&tSecurityAttributes))
	{
		return NULL;
	}
	
	HANDLE hMutex = INVALID_HANDLE_VALUE;
	if (strName.IsEmpty())
	{
		hMutex = CreateMutexA(&tSecurityAttributes, bInitialOwner, NULL);
	}
	else
	{
		hMutex = CreateMutexA(&tSecurityAttributes, bInitialOwner, strName);
	}
		
	FreeNullSecurityAttributes(&tSecurityAttributes);
	return hMutex;
}

/******************************************************************************
 * CreateEventAllAccess Function
 *****************************************************************************/
HANDLE CreateEventAllAccess(const CStringA& strName, BOOL bManualReset, BOOL bInitialState)
{
	/* Create an "all access" Security Attributes for the event */
	SECURITY_ATTRIBUTES tSecurityAttributes = {0};
	if (FALSE == CreateNullSecurityAttributes(&tSecurityAttributes))
	{
		return NULL;
	}
	
	HANDLE hEvent = INVALID_HANDLE_VALUE;
	if (strName.IsEmpty())
	{
		hEvent = CreateEventA(&tSecurityAttributes, bManualReset, bInitialState, NULL);
	}
	else
	{
		hEvent = CreateEventA(&tSecurityAttributes, bManualReset, bInitialState, strName);
	}

	FreeNullSecurityAttributes(&tSecurityAttributes);	
	return hEvent;
}

/******************************************************************************
 * SetProcessPrivilege Function
 *****************************************************************************/
BOOL SetProcessPrivilege(HANDLE hToken, LPCTSTR pszPrivilege, BOOL bEnablePrivilege)
{
	/* Get the LUID for the privilege */
	LUID tLuid = {0};
	if (FALSE == LookupPrivilegeValue(NULL, pszPrivilege, &tLuid))
	{
		DEBUG_PRINT_WIN32_ERROR("LookupPrivilegeValue");
		return FALSE;
	}

	/* Enable\Disable the requested privilege */
	TOKEN_PRIVILEGES tTokenPriv = {0};
    tTokenPriv.PrivilegeCount = 1;
    tTokenPriv.Privileges[0].Luid = tLuid;
    tTokenPriv.Privileges[0].Attributes = bEnablePrivilege ? SE_PRIVILEGE_ENABLED : 0;
    if (FALSE == AdjustTokenPrivileges(hToken, FALSE, &tTokenPriv, NULL, NULL, NULL))
    {
		DEBUG_PRINT_WIN32_ERROR("AdjustTokenPrivileges");
		return FALSE;
	}

	/* Make sure the last error isn't "ERROR_NOT_ALL_ASSIGNED" */
	if (GetLastError() != ERROR_SUCCESS)
	{
		DEBUG_PRINT_ERROR("AdjustTokenPrivileges returned %u", GetLastError());
		return FALSE;
	}

    return TRUE;
}

/******************************************************************************
 * EnableDebugPrivilege Function
 *****************************************************************************/
BOOL EnableDebugPrivilege()
{
	/* Get the current process' token */
	HANDLE hToken = NULL;
	if (FALSE == OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken))
	{
		DEBUG_PRINT_WIN32_ERROR("OpenProcessToken");
		return FALSE;
	}

	BOOL bRet = SetProcessPrivilege(hToken, SE_DEBUG_NAME, TRUE);
	CloseHandle(hToken);

	return bRet;
}



void DebugOnce(CStringA title)
{
#ifdef _DEBUG
	char pOut[100];
	_itoa_s(GetCurrentProcessId(), pOut, 100,10);
	CStringA name = "Global\\" + CStringA(pOut) + "_DebugOnce";
	HANDLE hEvent = CreateEventA(NULL, TRUE, TRUE, name);
	if (WaitForSingleObject(hEvent, 0) == WAIT_OBJECT_0)
	{
		MessageBoxA(NULL, "Attach DebugOnce", title, MB_OK);
	}
	ResetEvent(hEvent);

#endif
}



time_t  FileTimeToTimeT(FILETIME const& ft)   
{ 
	ULARGE_INTEGER ull;    
	ull.LowPart = ft.dwLowDateTime;    
	ull.HighPart = ft.dwHighDateTime;  
	return ull.QuadPart / 10000000ULL - 11644473600ULL; 
}

FILETIME  TimeTtoFileTime(time_t const& tt)
{
	ULARGE_INTEGER ull;
	ull.QuadPart = tt;
	ull.QuadPart += 11644473600ULL;
	ull.QuadPart = ull.QuadPart * 10000000ULL;
	FILETIME ret;
	ret.dwHighDateTime = ull.HighPart;
	ret.dwLowDateTime = ull.LowPart;
	return ret;
}