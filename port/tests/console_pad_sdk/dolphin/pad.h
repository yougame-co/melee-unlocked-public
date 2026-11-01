// Minimal SDK types for compiling the real PAD shim in a window-free test.
#pragma once
#include <stdint.h>
typedef int BOOL;
typedef uint32_t u32;
typedef int32_t s32;
typedef struct PADStatus {
    uint16_t button;
    int8_t stickX, stickY, substickX, substickY;
    uint8_t triggerLeft, triggerRight, analogA, analogB;
    int8_t err;
} PADStatus;
