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
int mu_console_has_save(void)
{
    return mu_host && mu_host->version >= 28 && mu_host->console_has_save &&
           mu_host->console_has_save();
}
void mu_console_log(const char* text)
{
    if (mu_host && mu_host->log) mu_host->log(text);
}
/* The host half of mu_console_frame_begin (shim/mu_console_pad.c): the agreed seed once, then the
 * host composes this body's confirmed pads. Returns 1 when a console frame began, 0 when none did, or
 * the host's restart code (-50 Friends CSS origin, -51 Doubles), which the caller must see unchanged. */
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
    return mu_host->console_frame_begin ? mu_host->console_frame_begin() : 0;
}
#ifdef MELEE_DOUBLES
static void (*doubles_restart_handler)(void);
void mu_doubles_set_restart(void (*restart)(void)) { doubles_restart_handler = restart; }
void mu_doubles_restart(void)
{
    if (!doubles_restart_handler) __builtin_trap();
    doubles_restart_handler();
}
#endif
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
    /* The Friends lobby's select: this player's pad drives its own seat's door only. */
    if (mu_lobby_css()) {
        mu_lobby_pads(status);
        for (mask = 0, i = 0; i < 4; i++)
            if (status[i].err == 0)
                mask |= 0x80000000u >> i;
    }
    if (mu_lobby_watch_pads(status)) mask = 0xF0000000u;   /* a guest watching the host's stage select */
    return mask;
}

/* PADClamp is the SDK's own (pad/PadClamp.c), compiled with the game. */

int mu_css_origin_host(int operation)
{
    const unsigned int version = operation == 16 || operation == 32 ? 30 : 29;
    return mu_host && mu_host->version >= version && mu_host->console_origin &&
           mu_host->console_origin(operation);
}

int mu_css_join_locked(void)
{
    return mu_host && mu_host->version >= 30 && mu_host->console_origin &&
           mu_host->console_origin(16);
}
