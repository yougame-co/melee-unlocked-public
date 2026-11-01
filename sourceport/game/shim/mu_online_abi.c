/* The host side of native online play, kept apart from the game headers (mu_online.c) as
 * mu_replay_abi.c is for replays. */
#include "mu_host.h"
#include "mu_shim.h"

int32_t mu_slippi_command(uint8_t command, const uint8_t* payload, uint32_t payload_size,
                          uint8_t* response, uint32_t response_capacity, uint32_t* response_size);

int mu_online_abi_command(unsigned int command, const unsigned char* payload, unsigned int size,
                          unsigned char* response, unsigned int capacity, unsigned int* response_size)
{
    return (int) mu_slippi_command((uint8_t) command, payload, size, response, capacity,
                                   (uint32_t*) response_size);
}

/* HUD sizes from the settings panel: stock percent | damage percent << 8 (0: host too old). */
unsigned int mu_hud_scales_abi(void)
{
    if (mu_host->version >= 15 && mu_host->hud_scales != NULL) {
        return mu_host->hud_scales();
    }
    return 0;
}

void mu_hud_player_abi(int slot, int present, int damage, int stocks, float tag_x, float tag_y,
                       int tag_visible)
{
    if (mu_host->version >= 15 && mu_host->hud_player != NULL) {
        mu_host->hud_player(slot, present, damage, stocks, tag_x, tag_y, tag_visible);
    }
}

void mu_hud_combo_abi(int slot, int link, float percent, int stunned)
{
    if (mu_host->version >= 21 && mu_host->hud_combo != NULL) {
        mu_host->hud_combo(slot, link, percent, stunned);
    }
}

void mu_online_abi_resim_phase(int entering)
{
    if (mu_host->version >= 13 && mu_host->resim_phase != NULL) {
        mu_host->resim_phase(entering);
    }
}

/* Nonzero when this run is an online test match; the game then boots straight into it. */
int mu_online_abi_test_match(int* mode, int* input_port)
{
    MuOnlineMatch match;
    int kind;
    if (mu_host->version < 13 || mu_host->online_test_match == NULL) {
        return 0;
    }
    kind = mu_host->online_test_match(&match);
    if (!kind) return 0;
    *mode = match.mode;
    *input_port = match.local_port;
    return kind;
}

void* mu_online_state_address(void);
unsigned int mu_online_state_size(void);

/* Appends this module's snapshot exclusions; returns how many it has. */
void* mu_online_audio_state_address(void);
unsigned int mu_online_audio_state_size(void);

uint32_t mu_online_state_exclusions(MuStateRegion* out, uint32_t capacity)
{
    /* The online bookkeeping and the online sound log: a load must restore neither. */
    if (out != NULL && capacity >= 1) {
        out[0].address = mu_online_state_address();
        out[0].size = mu_online_state_size();
    }
    if (out != NULL && capacity >= 2) {
        out[1].address = mu_online_audio_state_address();
        out[1].size = mu_online_audio_state_size();
    }
    return 2;
}

unsigned int mu_online_abi_retrace_count(void)
{
    return mu_host->vi_retrace_count();
}

void mu_online_abi_log(const char* text)
{
    mu_host->log(text);
}

/* Console adapter to the native predictor's RXB shape, with no Slippi device command. */
static void console_put32(uint8_t* p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static void console_wire(uint8_t* out, const MuPadStatus* p)
{
    out[0] = p->button >> 8; out[1] = p->button;
    out[2] = p->stick_x; out[3] = p->stick_y; out[4] = p->sub_x; out[5] = p->sub_y;
    out[6] = p->trigger_l; out[7] = p->trigger_r;
    out[8] = p->analog_a; out[9] = p->analog_b; out[10] = p->err; out[11] = 0;
}
int mu_console_abi(int operation, unsigned int body, unsigned char* local, unsigned char* rxb,
                   unsigned int* epoch, int* local_port)
{
    MuConsoleInputs inputs = {0};
    int r, k;
    int32_t smallest = (int32_t) body;
    if (mu_host->version < 26 || !mu_host->console_rollback ||
        !mu_host->console_rollback(operation, body, &inputs)) return 0;
    if (epoch) *epoch = inputs.epoch;
    if (local_port) *local_port = inputs.local_port;
    if (local) console_wire(local, &inputs.local);
    if (rxb) {
        /* RXB_RESULT=0, COUNT=1, DESYNC=2 (24 bytes), FRAMES=26, MIN=38, PADS=42. */
        for (k = 0; k < 297; ++k) rxb[k] = 0;
        rxb[0] = 1;
        for (r = 0; r < 3; ++r) {
            console_put32(rxb + 26 + 4 * r, (uint32_t) inputs.latest[r]);
            if (inputs.latest[r] < smallest) smallest = inputs.latest[r];
            for (k = 0; k < 7; ++k) console_wire(rxb + 42 + (r * 7 + k) * 12, &inputs.remote[r][k]);
        }
        console_put32(rxb + 38, (uint32_t) smallest);
    }
    return 1;
}
