/* GPL-3.0-or-later. Native presentation of slippi-ssbm-c d7174ca,
 * Scenes/Ranked/GameSetup.c and its Components directory. No rule/transport ownership. */
#ifndef MU_RANKED_WIDGETS_H
#define MU_RANKED_WIDGETS_H
#include <mu_ranked_setup_model.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { MU_SETUP_LEFT=1, MU_SETUP_RIGHT=2, MU_SETUP_DOWN=4, MU_SETUP_UP=8,
       MU_SETUP_A=16, MU_SETUP_B=32, MU_SETUP_X=64, MU_SETUP_Y=128, MU_SETUP_START=256 };
enum { MU_SETUP_TIMER_WHITE, MU_SETUP_TIMER_YELLOW, MU_SETUP_TIMER_RED };
typedef struct MuRankedSetupView {
    MuRankedSetupChoice draft, saved, history[5];
    unsigned step, initialized, confirming, button, dialog, picker;
    int cursor;
    unsigned char history_valid[5];
} MuRankedSetupView;
/* Portable presentation/input helpers; all legality comes from A1. */
unsigned mu_ranked_stage_material(unsigned stage);
unsigned mu_ranked_character_material(unsigned character);
unsigned mu_ranked_stock_frame(unsigned character, unsigned color);
unsigned mu_ranked_pad_buttons(unsigned long long raw);
int mu_ranked_disconnect_wait(unsigned* frames, unsigned down);
float mu_ranked_selector_x(unsigned index, unsigned count);
float mu_ranked_timeline_x(const MuRankedSetupModel*, unsigned step, unsigned item);
int mu_ranked_selector_move(unsigned enabled, unsigned count, int current, int direction);
unsigned mu_ranked_picker_move(unsigned current, unsigned direction);
unsigned mu_ranked_timer_text(char out[8], unsigned seconds, unsigned frames, int local);
void mu_ranked_view_sync(MuRankedSetupView*, const MuRankedSetupModel*);
/* Returns 1 only when OK requests submission. Caller sends draft through A1. */
int mu_ranked_view_input(MuRankedSetupView*, const MuRankedSetupModel*, unsigned down, unsigned repeat);
typedef struct MuRankedWidgets MuRankedWidgets;
MuRankedWidgets* mu_ranked_widgets_create(const MuRankedSetupModel*);
void mu_ranked_widgets_draw(MuRankedWidgets*, const MuRankedSetupModel*, const MuRankedSetupView*);
void mu_ranked_widgets_free(MuRankedWidgets*);
#ifdef __cplusplus
}
#endif
#endif
