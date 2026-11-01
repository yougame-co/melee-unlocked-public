/* GPL-3.0-or-later. Screen port of slippi-ssbm-c d7174ca,
 * Scenes/Ranked/GameSetup.c:17-171,630-842. A1 owns every packet, rule,
 * timeout, transport teardown, ready handshake and finalized match selection. */
#include <mu_native.h>
#include <mu_ranked_setup_online.h>
#include "mu_ranked_widgets.h"
#include <string.h>
#include <melee/gm/gm_1601.h>
#include <melee/gm/gm_1A36.h>
#include <melee/gm/gmscene.h>
#include <melee/lb/lbaudio_ax.h>

static MuRankedSetupView view;
static MuRankedWidgets* widgets;
static unsigned previous_status, panic_step;
static unsigned disconnect_frames, holding_disconnect;

static void disconnect_display(const MuRankedSetupModel* model, unsigned down)
{
    if (model->status != MU_SETUP_DISCONNECTED) return;
    /* A1 has already sent the abnormal completion and closed the connection.
     * Defer only its queued UI-scene exit, so the error remains readable. This
     * is the scene-loop flag (gm_801A4B60), never match/simulation state. */
    if (!holding_disconnect) {
        if (mu_gm_engine_state()->unk_C != 1) return;
        mu_gm_engine_state()->unk_C=0;
        holding_disconnect=1;
    }
    if (!mu_ranked_disconnect_wait(&disconnect_frames,down)) gm_801A4B60();
}

void mu_ranked_setup_load(void* data)
{
    const MuRankedSetupModel* model;
    (void)data;
    memset(&view, 0, sizeof view);
    previous_status = MU_SETUP_ACTIVE; panic_step = 255;
    disconnect_frames=holding_disconnect=0;
    mu_ranked_setup_begin();
    model = mu_ranked_setup_model();
    /* minor_load:22-33: a tiebreak goes straight through A1's handshake. */
    widgets = model->set && model->set->tiebreak_num && model->status != MU_SETUP_DISCONNECTED ?
        NULL : mu_ranked_widgets_create(model);
    mu_ranked_view_sync(&view, model);
    mu_ranked_widgets_draw(widgets, model, &view);
    disconnect_display(model,0);
    if (model->status == MU_SETUP_ACTIVE) lbAudioAx_80023F28(93);
}

void mu_ranked_setup_think(void)
{
    const MuRankedSetupModel* model = mu_ranked_setup_model();
    const MuRankedSetupStep* step;
    unsigned down, repeat;
    int port = gm_801677F0();
    mu_ranked_view_sync(&view, model);
    down = mu_ranked_pad_buttons(gm_GetButtonsTriggered(port));
    repeat = mu_ranked_pad_buttons(gm_801A36C0(port));
    if (mu_ranked_view_input(&view, model, down, repeat)) {
        if (mu_ranked_setup_submit(&view.draft, 0)) lbAudioAx_80024030(1);
    } else if (down || repeat) {
        step = mu_ranked_setup_current(model);
        if (step && step->player == model->local_player) lbAudioAx_80024030(2);
    }
    /* Publish half-filled strikes and dialog costumes BEFORE a timeout tick. */
    if (view.step == model->step && model->status == MU_SETUP_ACTIVE)
        mu_ranked_setup_set_draft(&view.draft);
    mu_ranked_setup_frame();
    mu_ranked_view_sync(&view, model);
    step = mu_ranked_setup_current(model);
    if (step && step->player == model->local_player && panic_step != model->step &&
        model->frames >= (unsigned)(step->seconds > 3 ? step->seconds-3 : 0)*60) {
        lbAudioAx_80024030(4); panic_step = model->step;
    }
    if (model->status == MU_SETUP_DISCONNECTED && previous_status != MU_SETUP_DISCONNECTED)
        lbAudioAx_80024030(3);
    previous_status = model->status;
    /* Tiebreaks ordinarily have no screen; a failed handshake still needs one. */
    if (!widgets && model->status == MU_SETUP_DISCONNECTED) widgets=mu_ranked_widgets_create(model);
    disconnect_display(model,down);
    mu_ranked_widgets_draw(widgets, model, &view);
}

void mu_ranked_setup_exit(void* data)
{
    (void)data;
    mu_ranked_setup_set_draft(NULL);
    mu_ranked_widgets_free(widgets); widgets = NULL;
    mu_ranked_setup_end();
}
