#pragma once
#include "pad.h"
typedef void (*VIRetraceCallback)(u32);
void VIWaitForRetrace(void);
u32 VIGetRetraceCount(void);
