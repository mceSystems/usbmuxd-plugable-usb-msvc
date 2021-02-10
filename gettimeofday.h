/******************************************************************************
 * gettimeofday.h
 * Taken from http://www.refcode.net/2013/02/gettimeofday-for-windows_13.html
*****************************************************************************/
#ifndef __GET_TIME_OF_DAY_H__
#define __GET_TIME_OF_DAY_H__

struct timezone
{
	int  tz_minuteswest; /* minutes W of Greenwich */
	int  tz_dsttime;     /* type of dst correction */
};

int gettimeofday(struct timeval *tv, struct timezone *tz);

#endif /* __GET_TIME_OF_DAY_H__ */