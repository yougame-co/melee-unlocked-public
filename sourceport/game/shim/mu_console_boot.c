/* Load the agreed session card without entering the memory-card scene. */
#include <melee/gm/gmmain_lib.h>
#include <melee/lb/lbcardgame.h>
#include <melee/lb/lbcardnew.h>
#include <sysdolphin/baselib/debug.h>

int mu_console_active(void);
int mu_console_has_save(void);
void mu_console_log(const char* text);

void mu_console_load_save(void)
{
    lbCardResult result;
    if (!mu_console_active() || !mu_console_has_save()) {
        return;
    }
    /* gmmain has initialized card/save state already. Reuse GS_MEMCARD's work
     * allocation, synchronous read (including native endian conversion and name
     * banks), and per-entry repair/audio-video setup, in that order. No archives,
     * prompt, scene body, input, or frame-zero barrier runs here. Native CARD
     * callbacks complete through the event queue, without a timed wait. */
    lbCardNew_AllocWorkArea();
    result = lb_8001CBBC();
    /* As GS_MEMCARD does (gmscmemcard.c:317): finalize with whatever the read returned; an
     * unreadable save leaves defaults. Every seat reads the same agreed bytes, and a seat that
     * still differs fails the frame-0 digest instead of crashing the game here. */
    if (result != LbCardResult_Ready && result != LbCardResult_2) {
        mu_console_log("console: the session save could not be read; starting with defaults");
    }
    gmMainLib_8015FA34(result);
}
