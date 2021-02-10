/******************************************************************************
 * DebugPrint.h
 *****************************************************************************/
#ifndef __MCE_DEBUG_PRINT_H__
#define __MCE_DEBUG_PRINT_H__
#include "windowsUtil.h"
typedef int(*com_plugin_log_type)(LPCSTR pLog);
extern com_plugin_log_type cp_log;

#if defined(_DEBUG) || defined(ALLOW_DEBUG_PRINT_ON_RELEASE)
	#define DEBUG_PRINT(fmt, ...) DebugPrint(MCE_MODULE, __FUNCTION__, __LINE__, "INFO", fmt, __VA_ARGS__)
	#define DEBUG_PRINT_HEX(buf,size) DebugPrintHex(MCE_MODULE, __FUNCTION__, __LINE__, "INFO", buf, size)
	#define DEBUG_PRINT_ERROR(fmt, ...) DebugPrint(MCE_MODULE, __FUNCTION__,  __LINE__, "ERROR", fmt, __VA_ARGS__)
	#define DEBUG_PRINT_WIN32_ERROR(func) DEBUG_PRINT_ERROR("%s has failed. Last error: %u", func, GetLastError())

	/******************************************************************************
	 * Includes
	 *****************************************************************************/
	#include <atlstr.h>
	#include <io.h>
	#include <fcntl.h>


	/******************************************************************************
	 * DebugPrint Function
	 *****************************************************************************/
	static void DebugPrint(LPCSTR pszModuleName, LPCSTR pszFunctionName, int iLine, LPCSTR pszLevel, LPCSTR pszMsgFormat, ...)
	{

		va_list args;
		va_start(args, pszMsgFormat);

		CStringA strLogMsg;
		strLogMsg.AppendFormat("[%u %u] [%s::%s::%d] %s: ", 
							   GetCurrentProcessId(),
							   GetCurrentThreadId(),
							   pszModuleName, 
							   pszFunctionName, 
							   iLine,
							   pszLevel);
		strLogMsg.AppendFormatV(pszMsgFormat, args);
		strLogMsg.Append("\n");

		OutputDebugStringA(strLogMsg);
		if (cp_log)
		{
			cp_log(strLogMsg.GetBuffer());
		}
		va_end(args);
	}

	/******************************************************************************
	 * DebugPrintHex Function
	 *****************************************************************************/
	static void DebugPrintHex(LPCSTR pszModuleName, LPCSTR pszFunctionName, int iLine, LPCSTR pszLevel, BYTE * pcBuffer, SIZE_T cbBufferSize)
	{

		CStringA strData;
		for (SIZE_T i = 0; i < cbBufferSize; i++)
		{
			strData.AppendFormat("%02X ", pcBuffer[i]);
		}
		DebugPrint(pszModuleName, pszFunctionName, iLine, pszLevel, strData);
	}

	/******************************************************************************
	 * ConsoleRedirectionThreadProc Function
	 *****************************************************************************/
	/* Disable the "This function or variable may be unsafe..." warning*/
	#pragma warning(push)
	#pragma warning(disable:4996) 
	static void __stdcall ConsoleRedirectionThreadProc(LPVOID lpParam)
	{
		HANDLE hReadyEvent = (HANDLE)lpParam;

		/* GUI process doesn't have a console by default, so we'll need to create one */
		#ifndef _CONSOLE
		if (AllocConsole())
		{
			ShowWindow(GetConsoleWindow(), SW_HIDE);
		}
		else
		{
			DEBUG_PRINT_WIN32_ERROR("AllocConsole");
		}
		#endif

		/* Create a pipe which will be used as our stdout and stderr */
		HANDLE hStdOutRead = NULL;
		HANDLE hStdOutWrite = NULL;
		if (FALSE == CreatePipe(&hStdOutRead, &hStdOutWrite, NULL, 0))
		{
			SetEvent(hReadyEvent);
			return;
		}
		
		/* Create CRT fds for our pipe */
		int iStdOutReadFd = _open_osfhandle((intptr_t)hStdOutRead, O_RDONLY | O_TEXT);
		int iStdOutWriteFd = _open_osfhandle((intptr_t)hStdOutWrite, O_RDONLY | O_TEXT);
		FILE * pStdOutReadFile = _fdopen(iStdOutReadFd, "r");
		
		/* Redirect stdout and stderr */
		freopen("CON", "w", stdout);
		freopen("CON", "w", stderr);
		setvbuf(stdout, NULL, _IONBF, 0);
		setvbuf(stderr, NULL, _IONBF, 0);
		dup2(iStdOutWriteFd, fileno(stdout));
		dup2(iStdOutWriteFd, fileno(stderr));
		SetStdHandle(STD_OUTPUT_HANDLE, hStdOutWrite);
		SetStdHandle(STD_ERROR_HANDLE, hStdOutWrite);

		/* Signal the calling thread we are redirecting the console output */
		SetEvent(hReadyEvent);

		/* Read lines from our pipe and write it to the debug output */
		CHAR szStdOut[4096] = { 0 };
		CStringA strDebugLine;
		while (fgets(szStdOut, STATIC_ARRAY_SIZE(szStdOut), pStdOutReadFile))
		{
			strDebugLine.Format("[%u %u] [%s] STDOUT: %s",
								GetCurrentProcessId(),
								GetCurrentThreadId(),
								MCE_MODULE,
								szStdOut);

			OutputDebugStringA(strDebugLine);
		}

		/* No real need to cleanup - we'll only die when the process terminates */
	}
	#pragma warning(pop)

	/******************************************************************************
	 * RedirectConsoleToDebug Function
	 *****************************************************************************/
	static void RedirectConsoleToDebug()
	{
		HANDLE hReadyEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
		if (FALSE == IS_VALID_HANDLE(hReadyEvent))
		{
			DEBUG_PRINT_WIN32_ERROR("CreateEvent");
			return;
		}

		CREATE_THREAD(ConsoleRedirectionThreadProc, hReadyEvent);
		WaitForSingleObject(hReadyEvent, INFINITE);
		
		SAFE_CLOSE_HANDLE(hReadyEvent);
	}

#else
	#define DEBUG_PRINT(fmt, ...)
	#define DEBUG_PRINT_ERROR(fmt, ...)
	#define DEBUG_PRINT_WIN32_ERROR(func)
#endif /* _DEBUG */

#endif /* __MCE_DEBUG_PRINT_H__ */