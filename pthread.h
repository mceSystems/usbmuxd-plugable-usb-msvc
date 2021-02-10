/******************************************************************************
 * pthread.h
 *****************************************************************************/
#ifndef __MCE_PTHREAD_H__
#define __MCE_PTHREAD_H__

/******************************************************************************
 * Includes
 *****************************************************************************/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>

#define pthread_mutex_t CRITICAL_SECTION
#define pthread_mutex_init(m,attr) InitializeCriticalSection(m)
#define pthread_mutex_destroy(m) DeleteCriticalSection(m)
#define pthread_mutex_lock(m) EnterCriticalSection(m)
#define pthread_mutex_unlock(m) LeaveCriticalSection(m)

#define pthread_t uintptr_t
#define pthread_create(thread,attr,start_routine,arg) *(thread) = _beginthread((start_routine), 0, (arg))

#endif /* __MCE_PTHREAD_H__ */