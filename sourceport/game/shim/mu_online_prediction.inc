/* Shared native predictor, included by mu_online.c and the window-free rollback harness.
 * Slippi policy stays on its original branches; console compares every controller field. */
static int load_opponent_inputs(PADStatus* stat, int frame, int predicting_allowed)
{
    int count, remote_port = 0, have_all = 1;
    u8* rxb = mu_online.rxb;
    for (count = 0; count < REMOTES; count++, remote_port++) {
        int latest;
        const u8* slots = rxb + RXB_OPNT_INPUTS + count * RXB_INPUTS_COUNT * PAD_SIZE;
        const u8* src = slots;   /* entry k is the pad of frame (latest - k) */
        if (remote_port == mu_online.local_index) {
            remote_port++;
        }
        latest = (int) be32(rxb + RXB_OPNT_FRAME_NUMS + 4 * count);
        if (latest - frame >= 0) {
            src = slots + (latest - frame) * PAD_SIZE;
        } else if (predicting_allowed && (mu_online.console_active || frame >= START_SYNC_FRAME - mu_online.delay) &&
                   !mu_online.game_over)
        {
            /* Predicting: the newest confirmed pad, or an early pad the other player sent for a
             * frame up to this one. Kept for comparison once the real input arrives. */
            u8 w = mu_online.predicted_write[count];
            const u8* early = early_pad(count, frame, latest);
            have_all = 0;
            if (early != NULL) {
                src = early;
                mu_online.early_used++;
            }
            memcpy(mu_online.predicted[count][w], src, PAD_SIZE);
            mu_online.predicted_early[count][w] = (u8) (early != NULL);
            mu_online.predicted_write[count] = (u8) ((w + 1) % PREDICTED_INPUTS_COUNT);
            if (!mu_online.player_predicting[count]) {
                mu_online.player_savestate_frame[count] = frame;
                mu_online.player_predicting[count] = 1;
                mu_online.predicted_read[count] = w;
                if (!mu_online.savestate_predicting) {
                    mu_online.savestate_frame = frame;
                    mu_online.savestate_predicting = 1;
                }
            }
        }
        if (remote_port < 4) {
            wire_to_pad(&stat[remote_port], src);
        }
    }
    if (have_all) {
        mu_online.finalized = frame;
    }
    return have_all;
}

static int used_slot(int frame)
{
    int slot = frame % USED_PADS_COUNT;
    return slot < 0 ? slot + USED_PADS_COUNT : slot;
}

static void record_used_pads(const PADStatus* stat, int frame, int confirmed)
{
    int slot = used_slot(frame), p;
    mu_online.used_frame[slot] = frame;
    mu_online.used_confirmed[slot] = (u8) confirmed;
    for (p = 0; p < 4; p++) {
        pad_to_wire(mu_online.used_pads[slot][p], &stat[p]);
    }
}

/* Whether two pads differ the way the game would act on (the comparison Slippi's prediction check
 * makes: the buttons it reads, both sticks, the triggers outside their dead zone). */
static int pads_differ(const u8* a, const u8* b)
{
    int t;
    if (mu_online.console_active) return memcmp(a, b, 11) != 0;
    if ((a[0] & 0x1F) != (b[0] & 0x1F) || (a[1] & 0x7F) != (b[1] & 0x7F) || memcmp(a + 2, b + 2, 4) != 0) {
        return 1;
    }
    for (t = 6; t < 8; t++) {
        if (a[t] <= 42 && b[t] <= 42) {
            continue;
        }
        if (a[t] != b[t]) {
            return 1;
        }
    }
    return 0;
}

/* Early inputs: a frame already run on a guess whose early pad, arrived since, says otherwise. The
 * game then rolls back now instead of when the confirmed pad comes, so the correction shows a few
 * milliseconds sooner and goes back fewer frames. The confirmed pad still decides: if it differs
 * from the early one, that is one more ordinary rollback. Returns 1 when such a frame exists. */
/* MELEE_EARLY_TRACE=1 (tests): every wrong early guess is logged with both pads. */
static int early_trace_on(void)
{
    static int value = -1;
    if (value < 0) {
        const char* v = getenv("MELEE_EARLY_TRACE");
        value = v != NULL && v[0] == '1';
    }
    return value;
}

/* MELEE_EARLY_ROLLBACK=1 turns it on; off by default (it adds rollbacks of its own). */
static int early_rollback_on(void)
{
    static int value = -1;
    if (value < 0) {
        const char* v = getenv("MELEE_EARLY_ROLLBACK");
        value = v != NULL && v[0] == '1';
    }
    return value;
}

/* Zone-level difference between two pads: a button edge, a stick crossing one of the game's
 * action lines, a shield drop, or a trigger crossing the light-shield line. Mirrors the host's
 * early-input judge (early_input_rules.h, all edges). */
static int side_of(int v, int line)
{
    return line < 0 ? v <= line : v >= line;
}
static int axis_crossed(int a, int b, const signed char* lines, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (side_of(a, lines[i]) != side_of(b, lines[i])) {
            return 1;
        }
    }
    return 0;
}
static int pads_contradict_zone(const u8* a, const u8* b)
{
    static const signed char main_x[4] = {-64, -23, 23, 64}, main_y[4] = {-56, -23, 23, 53};
    static const signed char c_x[4] = {-64, -23, 23, 64}, c_y[4] = {-53, -23, 23, 53};
    const unsigned int ba = ((unsigned int) a[0] << 8) | a[1], bb = ((unsigned int) b[0] << 8) | b[1];
    const int shielding_a = (ba & 0x0070) || a[6] >= 43 || a[7] >= 43;
    const int shielding_b = (bb & 0x0070) || b[6] >= 43 || b[7] >= 43;
    if ((ba ^ bb) & (0x1300 | 0x0C70)) {
        return 1;
    }
    if (axis_crossed((s8) a[2], (s8) b[2], main_x, 4) || axis_crossed((s8) a[3], (s8) b[3], main_y, 4) ||
        axis_crossed((s8) a[4], (s8) b[4], c_x, 4) || axis_crossed((s8) a[5], (s8) b[5], c_y, 4)) {
        return 1;
    }
    if ((shielding_a || shielding_b) && side_of((s8) a[3], -53) != side_of((s8) b[3], -53)) {
        return 1;
    }
    if ((a[6] >= 43) != (b[6] >= 43) || (a[7] >= 43) != (b[7] >= 43)) {
        return 1;
    }
    return 0;
}
/* MELEE_EARLY_RB_ANY=1: the old byte compare (any difference starts the early rollback). */
static int early_rb_any(void)
{
    static int value = -1;
    if (value < 0) {
        const char* v = getenv("MELEE_EARLY_RB_ANY");
        value = v != NULL && v[0] == '1';
    }
    return value;
}

static int early_contradicts(int count)
{
    int latest = (int) be32(mu_online.rxb + RXB_OPNT_FRAME_NUMS + 4 * count);
    int f = mu_online.player_savestate_frame[count];
    u8 r = mu_online.predicted_read[count];
    while (r != mu_online.predicted_write[count]) {
        const u8* early = early_pad(count, f, latest);
        if (early != NULL && (early_rb_any() ? pads_differ(early, mu_online.predicted[count][r])
                                               : pads_contradict_zone(early, mu_online.predicted[count][r]))) {
            return 1;
        }
        r = (u8) ((r + 1) % PREDICTED_INPUTS_COUNT);
        f++;
    }
    return 0;
}

/* Section 9: compare earlier predictions with inputs that have now arrived. Returns 1 when a
 * rollback was triggered. */
static int check_predictions(int frame)
{
    u8* rxb = mu_online.rxb;
    int rollback_required = mu_online.rollback_active;
    int count, savestate, values_set = 0;

    if (!mu_online.savestate_predicting) {
        mu_online.player_predicting[0] = mu_online.player_predicting[1] =
            mu_online.player_predicting[2] = 0;
        return 0;
    }
    for (count = 0; count < REMOTES; count++) {
        while (mu_online.player_predicting[count]) {
            int latest = (int) be32(rxb + RXB_OPNT_FRAME_NUMS + 4 * count);
            int ss = mu_online.player_savestate_frame[count];
            int offset = latest - ss;
            const u8* actual;
            const u8* predicted;
            int t, differ = 0;
            if (offset < 0) {
                break;   /* the inputs for the savestate frame have not arrived yet */
            }
            if (!mu_online.console_active && ss <= mu_online.stable_finalized) {
                if (latest > ss) {
                    goto inputs_match;   /* already finalized: advance without comparing */
                }
                break;
            }
            actual = rxb + RXB_OPNT_INPUTS + count * RXB_INPUTS_COUNT * PAD_SIZE + offset * PAD_SIZE;
            predicted = mu_online.predicted[count][mu_online.predicted_read[count]];
            if ((actual[0] & 0x1F) != (predicted[0] & 0x1F) ||
                (actual[1] & 0x7F) != (predicted[1] & 0x7F) ||
                memcmp(actual + 2, predicted + 2, 4) != 0)
            {
                differ = 1;
            }
            for (t = 6; t < 8 && !differ; t++) {
                if (actual[t] <= 42 && predicted[t] <= 42) {
                    continue;   /* both in the trigger dead zone */
                }
                if (actual[t] != predicted[t]) {
                    differ = 1;
                }
            }
            if (mu_online.console_active) differ = pads_differ(actual, predicted);
            mu_online.predictions++;
            if (mu_online.predicted_early[count][mu_online.predicted_read[count]]) {
                mu_online.early_scored[count]++;
                if (differ) {
                    mu_online.early_missed[count]++;
                }
                /* Honest play: at most 3.3% wrong in every run with 32 or more scored guesses (32 runs,
                 * 10-23); a peer sending made-up pads is 100% wrong. 24 guesses, over half wrong. */
                if (!mu_online.early_distrusted[count] && mu_online.early_scored[count] >= 24 &&
                    mu_online.early_missed[count] * 2 > mu_online.early_scored[count]) {
                    mu_online.early_distrusted[count] = 1;
                    logf_("online: remote %d early pads ignored for this match (%u of %u early guesses wrong)",
                          count, mu_online.early_missed[count], mu_online.early_scored[count]);
                }
                if (differ) {
                    mu_online.early_wrong++;
                    if (early_trace_on() && mu_online.early_wrong <= 300) {
                        logf_("early wrong: remote %d frame %d latest %d game frame %d guess %02x%02x %d,%d %d,%d %d %d actual %02x%02x %d,%d %d,%d %d %d",
                              count, ss, latest, frame,
                              predicted[0], predicted[1], (s8) predicted[2], (s8) predicted[3], (s8) predicted[4], (s8) predicted[5], predicted[6], predicted[7],
                              actual[0], actual[1], (s8) actual[2], (s8) actual[3], (s8) actual[4], (s8) actual[5], actual[6], actual[7]);
                    }
                } else {
                    mu_online.early_right++;
                }
            }
            if (differ) {
                mu_online.mispredictions++;
                rollback_required = 1;
                break;
            }
        inputs_match:
            mu_online.player_savestate_frame[count] = ss + 1;
            mu_online.predicted_read[count] =
                (u8) ((mu_online.predicted_read[count] + 1) % PREDICTED_INPUTS_COUNT);
            if (mu_online.predicted_read[count] == mu_online.predicted_write[count]) {
                break;   /* caught up with the prediction */
            }
        }
    }
    if (!mu_online.console_active && !rollback_required && early_rollback_on() && mu_online.rxb_size > RXB_SIZE) {
        for (count = 0; count < REMOTES; count++) {
            if (mu_online.player_predicting[count] == 1 && early_contradicts(count)) {
                rollback_required = 1;
                mu_online.early_rollbacks++;
                break;
            }
        }
    }
    /* The savestate frame is the lowest frame any remote is still predicted from. */
    savestate = mu_online.stable_finalized;
    for (count = 0; count < REMOTES; count++) {
        if (mu_online.player_predicting[count] != 1) {
            continue;
        }
        if (!values_set || mu_online.player_savestate_frame[count] < savestate) {
            savestate = mu_online.player_savestate_frame[count];
        }
        values_set = 1;
    }
    mu_online.savestate_frame = savestate;
    mu_online.finalized = mu_online.console_active ? savestate - 1 : savestate;
    if (savestate > (int) be32(rxb + RXB_SMALLEST_LATEST)) {
        mu_online.finalized = (int) be32(rxb + RXB_SMALLEST_LATEST);
    }
    if (rollback_required) {
        {
            unsigned long long mu_ticks_since_start(void);
            if (!mu_online.console_active) mu_online.rollback_start_ticks = mu_ticks_since_start();
        }
        if (!mu_online.console_active && trace_renew()) {
            unsigned long long mu_ticks_since_start(void);
            logf_("rollback trigger frame %d retrace %d tbms %d", frame,
                  (int) mu_online_abi_retrace_count(), (int) (mu_ticks_since_start() / 40500));
        }
        mu_online.selftest_rollback = 0;
        mu_online.rollback_active = 1;
        mu_online.rollback_should_load = 1;
        mu_online.rollback_end_frame = frame;
        mu_online.frame = frame + 1;
        mu_online.rollbacks++;
        if ((unsigned int) (frame - savestate) > mu_online.max_depth) {
            mu_online.max_depth = (unsigned int) (frame - savestate);
        }
        return 1;
    }
    for (count = 0; count < REMOTES; count++) {
        if (mu_online.player_predicting[count] == 1 &&
            mu_online.predicted_read[count] == mu_online.predicted_write[count])
        {
            mu_online.player_predicting[count] = 0;
        }
    }
    for (count = 0; count < REMOTES; count++) {
        if (mu_online.player_predicting[count] == 1) {
            return 0;
        }
    }
    mu_online.savestate_predicting = 0;
    return 0;
}

