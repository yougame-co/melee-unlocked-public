// SDK type surface only; the test compiles production mu_os.c for time/alarms/events.
#pragma once
#include "pad.h"
#include <stdarg.h>
typedef int64_t OSTime;
typedef uint32_t OSTick;
typedef struct OSContext { int unused; } OSContext;
typedef struct OSThread { int unused; } OSThread;
typedef struct OSThreadQueue { OSThread *head, *tail; } OSThreadQueue;
typedef int __OSInterrupt;
typedef uint32_t OSInterruptMask;
typedef void (*__OSInterruptHandler)(__OSInterrupt, OSContext*);
typedef int OSError;
typedef void (*OSErrorHandler)(void);
typedef struct OSCalendarTime {
    int sec, min, hour, mday, mon, year, wday, yday, msec, usec;
} OSCalendarTime;
typedef struct OSAlarm OSAlarm;
typedef void (*OSAlarmHandler)(OSAlarm*, OSContext*);
struct OSAlarm {
    OSAlarmHandler handler;
    uint32_t tag;
    OSTime fire;
    OSAlarm *prev, *next;
    OSTime period, start;
};
OSContext* OSGetCurrentContext(void);
void OSSetArenaLo(void*);
void OSSetArenaHi(void*);
