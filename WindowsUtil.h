/******************************************************************************
 * WindowsUtil.h
 *****************************************************************************/
#ifndef __MCE_WINDOWS_UTIL_H__
#define __MCE_WINDOWS_UTIL_H__

/******************************************************************************
 * Includes
 *****************************************************************************/
#include <process.h>
#include <atlstr.h>

/******************************************************************************
 * Macros
 *****************************************************************************/
/* Calculate the size of a statically defined array */
#define STATIC_ARRAY_SIZE(x) (sizeof((x)) / sizeof((x)[0]))

/* Get the size, in byte, of a null terminated wide string (including the null terminator) */
#define WIDE_STRING_SIZE_IN_BYTE(s) ((lstrlenW((s)) + 1) * sizeof(WCHAR))

/* Heap Allocations */
#define HEAP_ALLOC(type, size) (type *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size))
#define HEAP_ALLOC_ARRAY(type, itemsCount) HEAP_ALLOC(type, (itemsCount) * sizeof(type))
#define HEAP_REALLOC(type, p, size) (type *)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (LPVOID)(p), (size))
#define HEAP_FREE(p) HeapFree(GetProcessHeap(), 0, (void *)(p))
#define SAFE_HEAP_FREE(p) if (p)\
						  {\
							HEAP_FREE(p);\
							p = NULL;\
						  }

/* Handle Manipulation */
#define IS_VALID_HANDLE(h) ((NULL != (h)) && (INVALID_HANDLE_VALUE != (h)))
#define SAFE_CLOSE_HANDLE(h) if (IS_VALID_HANDLE(h))\
							 {\
								(void)CloseHandle(h);\
							 }\
							 h = NULL;

/* Thread Creation */
#define CREATE_THREAD_EX(lpSecAttrib,pfnThreadProc,lpParameter,dwCreationFlags,pdwThreadId) (HANDLE)_beginthreadex((LPSECURITY_ATTRIBUTES)(lpSecAttrib), \
																												   0, \
																												   (UINT ( __stdcall *)( void * ))pfnThreadProc, \
																												   (LPVOID)lpParameter, \
																												   dwCreationFlags, \
																												   (PUINT)pdwThreadId)
#define CREATE_THREAD(pfnThreadProc,lpParameter) CREATE_THREAD_EX(NULL, pfnThreadProc, lpParameter, 0, NULL)
#define EXIT_THREAD(retVal) _endthreadex((UINT)retVal);\
							return retVal; /* Prevents the compiler's error about not returning a value */

#define CHECK_ENV_VAR(key,val) TCHAR buf[MAX_PATH] = { 0 }; \
	if ((GetEnvironmentVariable(TEXT(key), buf, MAX_PATH)) && (0 == _tcsicmp(TEXT(val), buf))) \

/******************************************************************************
 * Types
 *****************************************************************************/
typedef struct _MCE_SHARED_MEMORY
{
	_MCE_SHARED_MEMORY() : hFileMapping(NULL), dwSize(0), pvMappedBuffer(nullptr){};
	HANDLE hFileMapping;
	DWORD dwSize;
	void * pvMappedBuffer;
} MCE_SHARED_MEMORY;

/******************************************************************************
 * CLastErrorProtector Class
 * Declare an CLastErrorProtector object at the beginning of a function in order 
 * to preserve the current thread's last error when the functions returns.
 * ***************************************************************************/
typedef DWORD (WINAPI * GetLastErrorType)(VOID);
typedef VOID (WINAPI * SetLastErrorType)(DWORD dwErrCode);

class CMceLastErrorProtector
{
public:
	CMceLastErrorProtector(GetLastErrorType pGetLastError, SetLastErrorType pSetLastError)//case of detour - should call original func
	{
		m_pGetLastError = pGetLastError;
		m_pSetLastError = pSetLastError;
		m_dwPreviousLastError = m_pGetLastError ? m_pGetLastError() : GetLastError();
		
	}
	CMceLastErrorProtector()
	{
		m_pGetLastError = NULL;
		m_pSetLastError = NULL;
		m_dwPreviousLastError = GetLastError();
	}
	~CMceLastErrorProtector()
	{
		m_pSetLastError ? m_pSetLastError(m_dwPreviousLastError) : ::SetLastError(m_dwPreviousLastError);
	}
	void SetLastError(DWORD le)
	{
		m_dwPreviousLastError = le;
	}
	DWORD LEP_GetLastError()
	{
		return m_dwPreviousLastError;
	}
private:
	DWORD m_dwPreviousLastError;
	GetLastErrorType m_pGetLastError;
	SetLastErrorType m_pSetLastError;

};

#define PROTECT_LAST_ERROR() CMceLastErrorProtector lastErrorProtect 
#define PROTECT_GET_LAST_ERROR(pGetLEProc,pSetLEProc,x) CMceLastErrorProtector lastErrorProtect((GetLastErrorType)pGetLEProc,(SetLastErrorType)pSetLEProc) ; DWORD x = lastErrorProtect.LEP_GetLastError() ;
/******************************************************************************
 * Exported Functions
 *****************************************************************************/
/******************************************************************************
 * CreateNullSecurityAttributes Function
 * Create a SECURITY_ATTRIBUTES which allows access to all.
 *****************************************************************************/
BOOL CreateNullSecurityAttributes(OUT LPSECURITY_ATTRIBUTES ptSecurityAttributes);

/******************************************************************************
 * FreeNullSecurityAttributes Function
 * Free memory allocated by CreateNullSecurityAttributes
 *****************************************************************************/
void FreeNullSecurityAttributes(LPSECURITY_ATTRIBUTES ptSecurityAttributes);

/******************************************************************************
 * CreateUuidString Function
 * Create a UUID string, similar to the output 
 *****************************************************************************/
BOOL CreateUuidString(OUT CStringA& strUuid);

/******************************************************************************
 * CreateUniqueObjectName Function
 * Creates a unique name for a kernel object, formatted as 
 * "MCE_OBJECT_{uuid}" or "Global\\MCE_OBJECT_{uuid}"
 *****************************************************************************/
CStringA CreateUniqueObjectName(BOOL bCreateInGlobal = TRUE);

/******************************************************************************
 * CreateSharedMemory Function
 *****************************************************************************/
BOOL CreateSharedMemory(const CStringA& strName, DWORD dwSize, OUT MCE_SHARED_MEMORY * ptSharedMem);

/******************************************************************************
 * CloseSharedMemory Function
 *****************************************************************************/
BOOL CloseSharedMemory(MCE_SHARED_MEMORY * ptSharedMem);

/******************************************************************************
 * CreateMutexAllAccess Function
 *****************************************************************************/
HANDLE CreateMutexAllAccess(const CStringA& strName = "", BOOL bInitialOwner = FALSE);

/******************************************************************************
 * CreateEventAllAccess Function
 *****************************************************************************/
HANDLE CreateEventAllAccess(const CStringA& strName = "", BOOL bManualReset = FALSE, BOOL bInitialState = FALSE);
							 
/******************************************************************************
 * SetProcessPrivilege Function
 * Enables\Disables privileges for the specified token. 
 * Based on an example from MSDN.
 *****************************************************************************/
BOOL SetProcessPrivilege(HANDLE hToken, LPCTSTR pszPrivilege, BOOL bEnablePrivilege);

/******************************************************************************
 * EnableDebugPrivilege Function
 * Enable debug privilege for the current process.
 *****************************************************************************/
BOOL EnableDebugPrivilege();
void DebugOnce(CStringA title);
time_t  FileTimeToTimeT(FILETIME const& ft); 
FILETIME  TimeTtoFileTime(time_t const& tt);

#endif /* __MCE_WINDOWS_UTIL_H__ */