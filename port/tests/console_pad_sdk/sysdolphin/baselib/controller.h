// Only the raw ring surface used by mu_pad.c. Renewal below is a test adapter to PADRead.
#pragma once
#include <dolphin/pad.h>
typedef struct HSD_PadData { PADStatus stat[4]; } HSD_PadData;
typedef struct PadLibData {
    int qread, qwrite, qcount;
    int qnum = 16;
    HSD_PadData queue[16]{};
} PadLibData;
extern PadLibData HSD_PadLibData;
void HSD_PadRenewRawStatus(BOOL);
