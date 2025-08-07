/*
	usbmuxd - iPhone/iPod Touch USB multiplex server daemon

Copyright (C) 2009	Hector Martin "marcan" <hector@marcansoft.com>
Copyright (C) 2009	Nikias Bassen <nikias@gmx.li>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 or version 3.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

*/

#ifndef __LOG_H__
#define __LOG_H__

#include "debugprint.h"

enum loglevel {
	LL_FATAL = 0,
	LL_ERROR,
	LL_WARNING,
	LL_NOTICE,
	LL_INFO,
	LL_DEBUG,
	LL_SPEW,
	LL_FLOOD,
	LL_MCE
};

static int log_level;

#define log_enable_syslog() 
#define log_disable_syslog() 

#if defined(_DEBUG) || defined(ALLOW_DEBUG_PRINT_ON_RELEASE)
	#define usbmuxd_log(level, fmt, ...) DebugPrint(MCE_MODULE, __FUNCTION__, __LINE__, #level, fmt, __VA_ARGS__)
	#define mce_log(fmt, ...) DebugPrint(MCE_MODULE, __FUNCTION__, __LINE__, "LL_MCE", fmt, __VA_ARGS__)
#else
	#define usbmuxd_log(level, fmt, ...)
#endif

#define __func__ __FUNCTION__

#endif
