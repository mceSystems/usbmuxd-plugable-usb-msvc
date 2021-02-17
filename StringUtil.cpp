/******************************************************************************
 * StringUtil.cpp
 *****************************************************************************/
/******************************************************************************
 * Includes
 *****************************************************************************/
#include "stdafx.h"
#include "StringUtil.h"

/******************************************************************************
 * Internal Functions Declarations
 *****************************************************************************/
static char HexDigitValue(char cDigit);

/******************************************************************************
 * CStringWToMultiByte Function
 *****************************************************************************/
static CStringA CStringWToMultiByte(CStringW strUnicode, UINT ulCodePage)
{
	CStringA sRet;

	int nSize = WideCharToMultiByte(ulCodePage, 0, strUnicode, strUnicode.GetLength()+1, NULL, NULL, NULL, NULL);
	if (nSize > 0)
	{
		WideCharToMultiByte(ulCodePage, 0, strUnicode, strUnicode.GetLength()+1, sRet.GetBuffer(nSize + 1), nSize + 1, NULL, NULL);
		sRet.ReleaseBuffer();
	}

	return sRet;
}

/******************************************************************************
 * CStringMultiByteToW Function
 *****************************************************************************/
static CStringW CStringMultiByteToW(CStringA strMultiByte, UINT ulCodePage)
{
	CStringW strRet;

	int nUnicodeSize = MultiByteToWideChar(ulCodePage, 0, strMultiByte, strMultiByte.GetLength()+1, NULL, NULL);
	if (nUnicodeSize > 0)
	{
		MultiByteToWideChar(ulCodePage, 0, strMultiByte, strMultiByte.GetLength()+1, strRet.GetBuffer(nUnicodeSize + 1), nUnicodeSize + 1);
		strRet.ReleaseBuffer();
	}

	return strRet;
}

/******************************************************************************
 * CStringWToUTF8 Function
 *****************************************************************************/
CStringA CStringWToUTF8(CStringW strUnicode)
{
	return CStringWToMultiByte(strUnicode, CP_UTF8);
}

/******************************************************************************
 * CStringUTF8ToW Function
 *****************************************************************************/
CStringW CStringUTF8ToW(CStringA strUTF8)
{
	return CStringMultiByteToW(strUTF8, CP_UTF8);
}

/******************************************************************************
 * CStringWToA Function
 *****************************************************************************/
CStringA CStringWToA(CStringW strUnicode)
{
	return CStringWToMultiByte(strUnicode, CP_ACP);
}

/******************************************************************************
 * CStringAToW Function
 *****************************************************************************/
CStringW CStringAToW(CStringA strAnsi)
{
	return CStringMultiByteToW(strAnsi, CP_ACP);
}

/******************************************************************************
 * CStringUTF8ToA Function
 *****************************************************************************/
CStringA CStringUTF8ToA(CStringA strUtf8)
{
	return CStringWToA(CStringUTF8ToW(strUtf8));
}

/******************************************************************************
 * GUIDToCStringW Function
 *****************************************************************************/
CStringW GUIDToCStringW(REFGUID tGuid)
{
	CStringW strClsid;
	StringFromGUID2(tGuid, strClsid.GetBuffer(GUID_STRING_SIZE), GUID_STRING_SIZE);
	strClsid.ReleaseBuffer();
	
	return strClsid;
}

/******************************************************************************
 * GUIDToCStringA Function
 *****************************************************************************/
CStringA GUIDToCStringA(REFGUID tGuid)
{
	return CStringWToA(GUIDToCStringW(tGuid));
}

/******************************************************************************
 * HexStringToBufferA Function
 *****************************************************************************/
bool HexStringToBufferA(CStringA& strHex, OUT BYTE * acBuffer, IN size_t cbBufferSize)
{
	/* Make sure the hex string is even */
	if (0 != (strHex.GetLength() % 2))
	{
		return false;
	}

	/* Make sure the buffer is big enough */
	size_t cbHexNeededBufferSize = strHex.GetLength() / 2;
	if (cbBufferSize < cbHexNeededBufferSize)
	{
		return false;
	}

	LPCSTR pszHex = strHex.GetBuffer();
	bool bRet = true;
	for (size_t i = 0; i < cbHexNeededBufferSize; i++)
	{
		char cHigh = HexDigitValue(pszHex[i * 2]);
		char cLow = HexDigitValue(pszHex[(i * 2) + 1]);
		if ((cHigh < 0) || (cLow < 0))
		{
			bRet = false;
			break;
		}

		acBuffer[i] = (cHigh << 4) | cLow;
	}

	strHex.ReleaseBuffer();
	return bRet;
}

/******************************************************************************
 * BufferToHexString Function
 * Note: Based on zframe_strhex from 0MQ.
 *****************************************************************************/
bool BufferToHexStringA(BYTE * pcBuffer, size_t cbBufferSize, OUT char * acHexString, IN size_t ccHexStringSize)
{
	static const char acHexChars[] = "0123456789ABCDEF";

	if (ccHexStringSize < ((cbBufferSize * 2) + 1))
	{
		return false;
	}

	for (size_t i = 0; i < cbBufferSize; i++)
	{
		acHexString[i * 2] = acHexChars[pcBuffer[i] >> 4];
		acHexString[(i * 2) + 1] = acHexChars[pcBuffer[i] & 0x0f];
	}
	acHexString[cbBufferSize * 2] = '\0';

	return true;
}

bool BufferToHexStringA(BYTE * pcBuffer, size_t cbBufferSize, OUT CStringA& strHex)
{
	size_t ccHexStringLength = cbBufferSize * 2;
	bool bRet = BufferToHexStringA(pcBuffer, cbBufferSize, strHex.GetBuffer(ccHexStringLength), ccHexStringLength + 1);
	strHex.ReleaseBuffer();
	return bRet;
}

/******************************************************************************
 * Internal Functions Implementations
 *****************************************************************************/
/******************************************************************************
 * HexDigitValue Function
 * Taken from UniCodeMultiByteTools.cpp (DS_CalcHexDigitValue)
 *****************************************************************************/
static char HexDigitValue(char cDigit)
{
	char cDigitValue = -1;
	if (cDigit >= '0' && cDigit <= '9')
	{
		cDigitValue = cDigit - '0';
	}
	else if (cDigit >= 'a' && cDigit <= 'f')
	{
		cDigitValue = 10 + cDigit - 'a';
	}
	else if (cDigit >= 'A' && cDigit <= 'F')
	{
		cDigitValue = 10 + cDigit - 'A';
	}
	return cDigitValue;
}