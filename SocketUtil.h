/******************************************************************************
 * SocketUtil.h
 *****************************************************************************/
#ifndef __MCE_SOCKET_UTIL_H__
#define __MCE_SOCKET_UTIL_H__

/******************************************************************************
 * Includes
 *****************************************************************************/
#include <WinSock2.h>
#include <Mstcpip.h>
#include "debugPrint.h"


/******************************************************************************
 * Defs & Macros
 *****************************************************************************/
#define LOCALHOST_ADDR ("127.0.0.1")

#define SAFE_CLOSE_SOCKET(s) if (INVALID_SOCKET != s)\
							 {\
								(void)shutdown(s, SD_BOTH);\
								(void)closesocket(s);\
								s = INVALID_SOCKET;\
							 }

#define DEBUG_PRINT_WSA_ERROR(func) DEBUG_PRINT_ERROR("%s has failed. Last error: %u", func, WSAGetLastError())

typedef enum _CONNECT_SOCKET_TYPE
{
	CONNECT_SOCKET_TYPE_TCP,
	CONNECT_SOCKET_TYPE_UDP
} CONNECT_SOCKET_TYPE;

typedef enum _SOCKET_EVENT
{
	SOCKET_EVENT_READ,
	SOCKET_EVENT_WRITE
} SOCKET_EVENT;

#define MS_TIMEOUT_TO_TIMEVAL(tv,ms_timeout) (tv).tv_sec = ms_timeout / 1000; \
											 (tv).tv_usec = (ms_timeout - (((tv).tv_sec) * 1000)) * 1000;


/******************************************************************************
 * CreateProxySocket Function
 *****************************************************************************/
static BOOL CreateListenSocket(OUT SOCKET 		* pListenSocket, 
							   IN OUT USHORT	* puPort, 
							   int 				iBacklog = SOMAXCONN)
{
	sockaddr_in localAddr = {0};

	/* Create the socket */
	SOCKET listenSocket = INVALID_SOCKET;
	listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (INVALID_SOCKET == listenSocket) 
	{
		DEBUG_PRINT_WSA_ERROR("socket");
        goto lblErrorCleanup;
    }
	
	/* Init the socket binding info */
	localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = inet_addr(LOCALHOST_ADDR);
    localAddr.sin_port = htons(*puPort);

	/* Bind the socket */
    if (SOCKET_ERROR == ::bind(listenSocket, (SOCKADDR *)&localAddr, sizeof (localAddr)))
	{
		DEBUG_PRINT_WSA_ERROR("bind");
		goto lblErrorCleanup;
    }

	/* Determine our bound port */
	int localAddrSize = sizeof(localAddr);
	if (SOCKET_ERROR == getsockname(listenSocket,(SOCKADDR *) &localAddr, &localAddrSize))
	{
		DEBUG_PRINT_WSA_ERROR("getsockname");
		goto lblErrorCleanup;
	}

	/* Start listening */
	if (SOCKET_ERROR == listen(listenSocket, iBacklog))
	{
		DEBUG_PRINT_WSA_ERROR("listen");
		goto lblErrorCleanup;
	}

	/* Store the socket and port. In case the original port was 0, this will store the binded 
	 * port number */
	*pListenSocket = listenSocket;
	*puPort = ntohs(localAddr.sin_port);
	return TRUE;

lblErrorCleanup:
	SAFE_CLOSE_SOCKET(listenSocket);

	return FALSE;
}

/******************************************************************************
 * ConnectSocket Function
 *****************************************************************************/
static SOCKET ConnectSocket(LPCSTR pszAddress, WORD wPort, CONNECT_SOCKET_TYPE eType)
{
	SOCKET hNewSocket = INVALID_SOCKET;
	if (CONNECT_SOCKET_TYPE_TCP == eType)
	{
		hNewSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	}
	else if (CONNECT_SOCKET_TYPE_UDP == eType)
	{
		hNewSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	}
	else
	{
		DEBUG_PRINT_ERROR("Invalid connect socket type: %d", eType);
		return INVALID_SOCKET;
	}

	if (INVALID_SOCKET == hNewSocket)
	{
		DEBUG_PRINT_WSA_ERROR("socket");
		return INVALID_SOCKET;
	}

	/* Create connect's params */
	sockaddr_in connectAddr;
	connectAddr.sin_family = AF_INET;
	connectAddr.sin_addr.s_addr = inet_addr(pszAddress);
	connectAddr.sin_port = htons(wPort);

	/* Connect the socket */
	if (SOCKET_ERROR == connect(hNewSocket, (SOCKADDR *)&connectAddr, sizeof(connectAddr)))
	{
		DEBUG_PRINT_WSA_ERROR("connect");
		(void)closesocket(hNewSocket);
		return INVALID_SOCKET;
	}

	return hNewSocket;
}

/******************************************************************************
 * SocketRecvAll Function
 *****************************************************************************/
static BOOL SocketSendAll(SOCKET hSock, const char * pcBuffer, int iLen)
{
	int iBytesSent = 0;
	int iBytesLeft = iLen;
	int iResult = 0;

	while (iBytesSent < iLen)
	{
		iResult = send(hSock, pcBuffer + iBytesSent, iBytesLeft, 0);
		if (SOCKET_ERROR == iResult)
		{
			DEBUG_PRINT_WSA_ERROR("send");
			return FALSE;
		}
				
		iBytesSent += iResult;
		iBytesLeft -= iResult;
	}

	return TRUE;
}

/******************************************************************************
 * SocketRecvAll Function
 *****************************************************************************/
static BOOL SocketRecvAll(SOCKET hSock, char * pcBuffer, int iLen)
{
	int iBytesReceived = 0;
	int iBytesLeft = iLen;
	int iResult = 0;

	while (iBytesReceived < iLen)
	{
		iResult = recv(hSock, pcBuffer + iBytesReceived, iBytesLeft, 0);
		if (SOCKET_ERROR != iResult)
		{
			if (0 == iResult)
			{
				/* The socket was closed */
				return FALSE;
			}

			iBytesReceived += iResult;
			iBytesLeft -= iResult;
		}
		else
		{
			/* Skip timeout errors */
			if (WSAGetLastError() != WSAETIMEDOUT)
			{
				DEBUG_PRINT_WSA_ERROR("recv");
				return FALSE;
			}
		}
	}
	
	return TRUE;
}

/******************************************************************************
 * SocketWaitForEvent Function
 * Note: dwTimeout is in milliseconds.
 *****************************************************************************/
static BOOL SocketWaitForEvent(SOCKET hSock, DWORD dwTimeout, SOCKET_EVENT eEvent)
{
	/* Initialize an fdset for the listening socket*/
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(hSock, &fds);

	timeval * ptSelectTimeout = NULL;
	timeval tTimeout = { 0 };
	if (INFINITE != dwTimeout)
	{		
		MS_TIMEOUT_TO_TIMEVAL(tTimeout, dwTimeout);
		ptSelectTimeout = &tTimeout;
	}

	/* Wait for a socket "event" */
	int iResult = SOCKET_ERROR;
	switch (eEvent)
	{
	case SOCKET_EVENT_READ:
		iResult = select(0, &fds, NULL, NULL, ptSelectTimeout);
		break;

	case SOCKET_EVENT_WRITE:
		iResult = select(0, NULL, &fds, NULL, ptSelectTimeout);
		break;

	default:
		//LOG_ERROR("Invalid socket event: %d", eEvent);
		return FALSE;
	}
	
	if (iResult < 1)
	{
		if (SOCKET_ERROR == iResult)
		{
			//DEBUG_PRINT_WSA_ERROR("select");
		}

		/* Timeout */
		return FALSE;
	}

	return TRUE;
}


/******************************************************************************
 * SocketAcceptWithTimeout Function
 * Note: dwTimeout is in milliseconds, can be INFINITE.
*****************************************************************************/
static BOOL SocketAcceptWithTimeout(SOCKET hListeningSocket, DWORD dwTimeout, OUT SOCKET * phNewClientSocket)
{
	if (NULL == phNewClientSocket)
	{
		return FALSE;
	}

	/* Wait for a new connection */
	if (FALSE == SocketWaitForEvent(hListeningSocket, dwTimeout, SOCKET_EVENT_READ))
	{
		return FALSE;
	}

	/* Accept the new connection */
	*phNewClientSocket = accept(hListeningSocket, NULL, NULL);
	if (INVALID_SOCKET == *phNewClientSocket)
	{
		DEBUG_PRINT_WSA_ERROR("accept");
		return FALSE;
	}

	return TRUE;
}

/******************************************************************************
 * SocketRecvAllWithTimeout Function
 * hSock should be a non blocking socket
 *****************************************************************************/
static BOOL SocketRecvAllWithTimeout(SOCKET hSock, char * pcBuffer, int iLen, DWORD dwTimeout)
{
	int iBytesReceived = 0;
	int iBytesLeft = iLen;
	int iResult = 0;
	
	DWORD dwStartTime = GetTickCount();
	DWORD dwTotalTime = 0;
	while (iBytesReceived < iLen)
	{
		if (INFINITE == dwTimeout)
		{
			if (FALSE == SocketWaitForEvent(hSock, INFINITE, SOCKET_EVENT_READ))
			{
				/* Error */
				return FALSE;
			}
		}
		else
		{
			if (dwTotalTime >= dwTimeout)
			{
				/* Timeout */
				return FALSE;
			}

			if (FALSE == SocketWaitForEvent(hSock, dwTimeout - dwTotalTime, SOCKET_EVENT_READ))
			{
				/* Timeout */
				return FALSE;
			}
		}

		iResult = recv(hSock, pcBuffer + iBytesReceived, iBytesLeft, 0);
		if (SOCKET_ERROR == iResult)
		{
			DEBUG_PRINT_WSA_ERROR("recv");
			return FALSE;
		}

		iBytesReceived += iResult;
		iBytesLeft -= iResult;	
		dwTotalTime = GetTickCount() - dwStartTime;
	}

	return TRUE;
}

/******************************************************************************
 * SocketSendAllWithTimeout Function
 *****************************************************************************/
static BOOL SocketSendAllWithTimeout(SOCKET hSock, const char * pcBuffer, int iLen, DWORD dwTimeout)
{
	int iBytesSent = 0;
	int iBytesLeft = iLen;
	int iResult = 0;

	DWORD dwStartTime = GetTickCount();
	DWORD dwTotalTime = 0;
	while (iBytesSent < iLen)
	{
		if (INFINITE == dwTimeout)
		{
			if (FALSE == SocketWaitForEvent(hSock, INFINITE, SOCKET_EVENT_WRITE))
			{
				/* Error */
				return FALSE;
			}
		}
		else
		{
			if (dwTotalTime >= dwTimeout)
			{
				/* Timeout */
				return FALSE;
			}

			if (FALSE == SocketWaitForEvent(hSock, dwTimeout - dwTotalTime, SOCKET_EVENT_WRITE))
			{
				/* Timeout */
				return FALSE;
			}
		}

		iResult = send(hSock, pcBuffer + iBytesSent, iBytesLeft, 0);
		if (SOCKET_ERROR == iResult)
		{
			DEBUG_PRINT_WSA_ERROR("send");
			return FALSE;
		}
				
		iBytesSent += iResult;
		iBytesLeft -= iResult;
		dwTotalTime = GetTickCount() - dwStartTime;
	}

	return TRUE;
}

/******************************************************************************
 * SocketSetTimeouts Function
 * Note: dwTimeout is in milliseconds.
 *****************************************************************************/
static BOOL SocketSetTimeouts(SOCKET hSock, DWORD dwRecvTimeout, DWORD dwSendTimeout)
{
	if (SOCKET_ERROR == setsockopt(hSock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&dwRecvTimeout, sizeof(dwRecvTimeout)) ||
		SOCKET_ERROR == setsockopt(hSock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&dwSendTimeout, sizeof(dwSendTimeout)))
	{
		DEBUG_PRINT_WSA_ERROR("setsockopt");
		return FALSE;
	}

	return TRUE;
}

/******************************************************************************
 * SocketSetNonBlocking Function
 *****************************************************************************/
static BOOL SocketSetNonBlocking(SOCKET hSock, BOOL bNonBlocking)
{
	ULONG ulNonBlockingMode = bNonBlocking;
	if (SOCKET_ERROR == ioctlsocket(hSock, FIONBIO, &ulNonBlockingMode))
	{
		//DEBUG_PRINT_WSA_ERROR("ioctlsocket");
		return FALSE;
	}

	return TRUE;
}

/******************************************************************************
 * SocketEnableKeepAlive Function
 *****************************************************************************/
static BOOL SocketEnableKeepAlive(SOCKET hSock, ULONG ulTime, ULONG ulInterval)
{
	/* Initialize the keep alive struct */
	struct tcp_keepalive tKeepAlive = { 0 };
	tKeepAlive.onoff = 1;
	tKeepAlive.keepalivetime = ulTime;
	tKeepAlive.keepaliveinterval = ulInterval;

	DWORD cbBytesReturned = 0;
	if (SOCKET_ERROR == WSAIoctl(hSock,					/* Socket */
								 SIO_KEEPALIVE_VALS, 	/* IOCTL code */
								 &tKeepAlive, 			/* In buffer */
								 sizeof(tKeepAlive),	/* In buffer size */
								 NULL, 					/* Out buffer */
								 0, 					/* Out buffer size */
								 &cbBytesReturned, 		/* Bytes return (in out buffer) */
								 NULL, 					/* Overlapped */
								 NULL))					/* Completion routine */
	{
		DEBUG_PRINT_WSA_ERROR("WSAIoctl");
		return FALSE;
	}

	return TRUE;

}

/******************************************************************************
 * SocketDisableKeepAlive Function
 *****************************************************************************/
static BOOL SocketDisableKeepAlive(SOCKET hSock)
{
	/* Initialize the keep alive struct */
	struct tcp_keepalive tKeepAlive = { 0 };

	DWORD cbBytesReturned = 0;
	if (SOCKET_ERROR == WSAIoctl(hSock,					/* Socket */
								 SIO_KEEPALIVE_VALS, 	/* IOCTL code */
								 &tKeepAlive, 			/* In buffer */
								 sizeof(tKeepAlive),	/* In buffer size */
								 NULL, 					/* Out buffer */
								 0, 					/* Out buffer size */
								 &cbBytesReturned, 		/* Bytes return (in out buffer) */
								 NULL, 					/* Overlapped */
								 NULL))					/* Completion routine */
	{
		DEBUG_PRINT_WSA_ERROR("WSAIoctl");
		return FALSE;
	}

	return TRUE;

}

/******************************************************************************
 * SocketDisableTcpDelay Function
 *****************************************************************************/
static BOOL SocketDisableTcpDelay(SOCKET hSock)
{
	BOOL bTcpNoDelay = TRUE;
	if (0 != setsockopt(hSock, IPPROTO_TCP, TCP_NODELAY, (char *)&bTcpNoDelay, sizeof(bTcpNoDelay)))
	{
		DEBUG_PRINT_WSA_ERROR("setsockopt");
		return FALSE;
	}

	return TRUE;
}

#ifdef __cplusplus

namespace MCE
{
	/******************************************************************************
	 * CTCPSocket Class
	 *****************************************************************************/
	class CTCPSocket
	{
		/******************************************************************************
		 * Public Methods
		 *****************************************************************************/
	public:
		/******************************************************************************
		 * Ctor & Dtor
		 *****************************************************************************/
		CTCPSocket(SOCKET hSock, bool bIsCurrentlyNonBlocking = false) :
			m_hSock(hSock), m_bIsNonBlocking(bIsCurrentlyNonBlocking)
		{
		}

		~CTCPSocket()
		{
			SAFE_CLOSE_SOCKET(m_hSock);
		}

		/******************************************************************************
		 * ConnectSocket Method
		 *****************************************************************************/
		static CTCPSocket * ConnectSocket(LPCSTR pszAddress, WORD wPort)
		{
			SOCKET hSock = ::ConnectSocket(pszAddress, wPort, CONNECT_SOCKET_TYPE_TCP);
			if (INVALID_SOCKET == hSock)
			{
				return NULL;
			}

			return new CTCPSocket(hSock, false);
		}

		/******************************************************************************
		 * Send Method
		 *****************************************************************************/
		bool Send(const char * pcBuffer, int iLen, OUT int * piBytesSent)
		{
			*piBytesSent = send(m_hSock, pcBuffer, iLen, 0);
			return (*piBytesSent) > 0;
		}

		/******************************************************************************
		 * SendAll Method
		 *****************************************************************************/
		bool SendAll(const char * pcBuffer, int iLen)
		{
			if (m_bIsNonBlocking)
			{
				return (FALSE != SocketSendAllWithTimeout(m_hSock, pcBuffer, iLen, INFINITE));
			}

			return (FALSE != SocketSendAll(m_hSock, pcBuffer, iLen));
		}

		bool SendAll(const char * pcBuffer, int iLen, DWORD dwTimeout)
		{
			if (m_bIsNonBlocking)
			{
				return false;
			}

			return (FALSE != SocketSendAllWithTimeout(m_hSock, pcBuffer, iLen, dwTimeout));
		}

		/******************************************************************************
		 * Recv Method
		 *****************************************************************************/
		bool Recv(char * pcBuffer, int iLen, OUT int * piBytesRead)
		{
			*piBytesRead = recv(m_hSock, pcBuffer, iLen, 0);
			return (*piBytesRead) > 0;
		}

		/******************************************************************************
		 * RecvAll Method
		 *****************************************************************************/
		bool RecvAll(char * pcBuffer, int iLen)
		{
			if (m_bIsNonBlocking)
			{
				return (FALSE != SocketRecvAllWithTimeout(m_hSock, pcBuffer, iLen, INFINITE));
			}

			return (FALSE != SocketRecvAll(m_hSock, pcBuffer, iLen));
		}

		bool RecvAll(char * pcBuffer, int iLen, DWORD dwTimeout)
		{
			if (m_bIsNonBlocking)
			{
				return false;
			}

			return (FALSE != SocketRecvAllWithTimeout(m_hSock, pcBuffer, iLen, dwTimeout));
		}
		BOOL Purge(DWORD& out)
		{
			out = 0;
			BOOL bRet = FALSE;
			fd_set set;
			struct timeval timeout;
			FD_ZERO(&set); /* clear the set */
			FD_SET(m_hSock, &set); /* add our file descriptor to the set */
			timeout.tv_sec = 0;
			timeout.tv_usec = 0;

			int iResult = SOCKET_ERROR;
			iResult = select(0, &set, NULL, NULL, &timeout);
			if (iResult != SOCKET_ERROR)
			{
				bRet = TRUE;
				if (iResult > 0)
				{
					char pBuff[1024];
					iResult = recv(m_hSock, pBuff, 1024, 0);
					out = iResult;
				}

			}
			return bRet;
		}
		/******************************************************************************
		 * Non-Blocking mode Methods
		 *****************************************************************************/
		bool SetNonBlocking(bool bIsNonBlocking)
		{
			if (m_bIsNonBlocking == bIsNonBlocking)
			{
				return true;
			}

			return (FALSE != SocketSetNonBlocking(m_hSock, bIsNonBlocking));
		}
		bool IsNonBlocking() const
		{
			return m_bIsNonBlocking;
		}

		/******************************************************************************
		 * Keep-Alive
		 *****************************************************************************/
		bool EnableKeepAlive(ULONG ulTime, ULONG ulInterval)
		{
			return (FALSE != SocketEnableKeepAlive(m_hSock, ulTime, ulInterval));
		}

		bool DisableKeepAlive()
		{
			return (FALSE != SocketDisableKeepAlive(m_hSock));
		}

		/******************************************************************************
		 * TCP Delay
		 *****************************************************************************/
		bool DisableTcpDelay()
		{
			return (FALSE != SocketDisableTcpDelay(m_hSock));
		}

		/******************************************************************************
		 * Timeouts
		 *****************************************************************************/
		bool SetTimeouts(DWORD dwRecvTimeout, DWORD dwSendTimeout)
		{
			if (m_bIsNonBlocking)
			{
				return false;
			}

			return (FALSE != SocketSetTimeouts(m_hSock, dwRecvTimeout, dwSendTimeout));
		}

		/******************************************************************************
		 * Cast Operators
		 *****************************************************************************/
		operator SOCKET()
		{
			return m_hSock;
		}

		/******************************************************************************
		 * Private Fields
		 *****************************************************************************/
	private:
		SOCKET m_hSock;
		bool m_bIsNonBlocking;
	};

	/******************************************************************************
	 * CListeningSocket Class
	 *****************************************************************************/
	class CListeningSocket
	{
		/******************************************************************************
		 * Public Methods
		 *****************************************************************************/
	public:
		CListeningSocket(WORD wPort = 0) : 
			m_wPort(wPort), m_hSock(INVALID_SOCKET)
		{
		}
		~CListeningSocket()
		{
			SAFE_CLOSE_SOCKET(m_hSock);
		}

		/******************************************************************************
		 * Start Method
		 *****************************************************************************/
		bool Start(int iBacklog = SOMAXCONN)
		{
			/* Make sure Start wasn't already called */
			if (INVALID_SOCKET != m_hSock)
			{ 
				return false;
			}

			if (FALSE == CreateListenSocket(&m_hSock, &m_wPort, iBacklog))
			{
				return false;
			}

			return true;
		}

		/******************************************************************************
		 * Accept Method
		 *****************************************************************************/
		CTCPSocket * Accpet(DWORD dwTimeout = INFINITE)
		{
			if (INVALID_SOCKET == m_hSock)
			{
				return NULL;
			}

			SOCKET hNewSock = INVALID_SOCKET;
			if (FALSE == SocketAcceptWithTimeout(m_hSock, dwTimeout, &hNewSock))
			{
				return NULL;
			}

			return new CTCPSocket(hNewSock);
		}

		/******************************************************************************
		 * Close Method
		 *****************************************************************************/
		void Close()
		{
			SAFE_CLOSE_SOCKET(m_hSock);
		}

		WORD GetPort() const
		{
			return m_wPort;
		}

	private:
		SOCKET m_hSock;
		WORD m_wPort;
	};
}
#endif

 #endif /* __MCE_SOCKET_UTIL_H__ */