/* Controllers. The host's input layer (every device kind, rebinding, auto L-cancel) produces the four
 * pads; this copies them into the game's structures. */
#include <dolphin/pad.h>

#include "mu_shim.h"
#include <string.h>

BOOL PADInit(void) { return 1; }
int mu_console_active(void)
{
    uint32_t seed;
    return mu_host->console_seed && mu_host->console_seed(&seed);
}
/* The host half of mu_console_frame_begin (shim/mu_console_pad.c): the agreed seed once, then the
 * host composes this body's confirmed pads. Returns whether a console frame began. */
int mu_console_host_frame_begin(void)
{
    static int seeded;
    uint32_t seed;
    void mu_apply_seed(uint32_t seed);
    /* main may initialize the SDK RNG during boot; frame zero takes the agreed seed again,
     * before the host snapshots it or any scene body consumes random values. */
    if (!seeded && mu_host->console_seed && mu_host->console_seed(&seed)) {
        mu_apply_seed(seed);
        seeded = 1;
    }
    return mu_host->console_frame_begin && mu_host->console_frame_begin();
}
int PADReset(u32 mask) { (void) mask; return 1; }
BOOL PADRecalibrate(u32 mask) { (void) mask; return 1; }
void PADSetSamplingRate(u32 msec) { (void) msec; }
void PADSetSpec(u32 spec) { (void) spec; }

/* PADControlMotor(chan, command): 0 stop, 1 rumble, 2 stop hard. */
void PADControlMotor(s32 chan, u32 command)
{
    /* Online, only the local player's port rumbles, on its physical port (shim/mu_online_audio.c). */
    s32 port = mu_online_motor_port(chan, command);
    if (port >= 0) {
        mu_host->pad_rumble(port, command == 1);
    }
}

/* A motor the online code sets itself, when a rollback or the match ends. */
void mu_pad_motor(int port, int on) { mu_host->pad_rumble(port, on); }

u32 PADRead(PADStatus* status)
{
    MuPadStatus pads[4];
    u32 mask = 0;
    int i;
    mu_host->pad_read(pads);
    for (i = 0; i < 4; i++) {
        /* The padding byte after err too: copied into the pad queues, it would hold stack bytes. */
        memset(&status[i], 0, sizeof status[i]);
        status[i].button = pads[i].button;
        status[i].stickX = pads[i].stick_x;
        status[i].stickY = pads[i].stick_y;
        status[i].substickX = pads[i].sub_x;
        status[i].substickY = pads[i].sub_y;
        status[i].triggerLeft = pads[i].trigger_l;
        status[i].triggerRight = pads[i].trigger_r;
        status[i].analogA = pads[i].analog_a;
        status[i].analogB = pads[i].analog_b;
        status[i].err = pads[i].err;
        if (pads[i].err == 0)
            mask |= 0x80000000u >> i;
    }
    return mask;
}

/* PADClamp is the SDK's own (pad/PadClamp.c), compiled with the game. */
