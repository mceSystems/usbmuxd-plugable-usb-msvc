/******************************************************************************
 * dllmain.cpp
 *****************************************************************************/
/******************************************************************************
 * Includes
 *****************************************************************************/
#include "stdafx.h"


/******************************************************************************
 * Exported Functions Implementations
 *****************************************************************************/
/******************************************************************************
 * DllMain Function
 *****************************************************************************/
BOOL APIENTRY DllMain(HMODULE	hModule,
					  DWORD		ulReasonForCall,
					  LPVOID	lpReserved)
{
	switch (ulReasonForCall)
	{
	case DLL_PROCESS_ATTACH:
		(VOID)DisableThreadLibraryCalls(hModule);
		break;

	case DLL_PROCESS_DETACH:
		break;
	}

	return TRUE;
}