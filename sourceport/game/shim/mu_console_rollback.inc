/* Console lifecycle around the existing predictor and B1/B2 runner. Included after camera_tasks.
 * No Slippi setup, mode, rule, RNG, reporting or recording operations belong here. */
int mu_console_rollback_active(void) { return mu_online.console_active; }
/* Own admission/alarm rumble from setup onward, including the first body's pre-checkpoint gate. */
int mu_console_rumble_owned(void) { return mu_online.console_setup || mu_online.console_active; }

unsigned int mu_console_rollback_codes(void)
{
    if (!mu_online.console_setup && !mu_online.console_active) return 0;
    return MU_RC_INIT_STAGE_DATA | MU_RC_INIT_PLAYER_DATA | MU_RC_OFFSCREEN_DAMAGE |
           MU_RC_DEAD_UP_FALL | MU_RC_FD_BG_SEED | MU_RC_PS_ZERO_BUFFER | MU_RC_PS_IS_VALID |
           MU_RC_PS_FILE_LOAD | MU_RC_WHISPY_FIX | MU_RC_NANA_DETERMINISM | MU_RC_PS_MONITOR;
}

/* Before fighter/stage setup; deterministic initialization without Slippi's gameplay rules. */
void mu_console_match_setup(void)
{
    int i;
    if (!mu_console_abi(CONSOLE_QUERY, 0, NULL, NULL, NULL, NULL)) return;
    if (!gm_804D6720 || (gm_804D6720->scene_kind != GS_VS &&
                         gm_804D6720->scene_kind != GS_SUDDEN_DEATH)) return;
    memset(&mu_online, 0, sizeof mu_online);
    mu_online.console_setup = 1;
    mu_online.finalized = mu_online.stable_finalized = -1;
    for (i = 0; i < USED_PADS_COUNT; ++i) mu_online.used_frame[i] = -1;
}

static void console_fail(int frame, const char* why)
{
    logf_("console: rollback epoch %u body %d failed: %s", mu_online.console_epoch, frame, why);
    if (mu_online.console_active) mu_online_audio_match_exit();
    mu_console_abi(CONSOLE_FAIL, (u32) frame, NULL, NULL, NULL, NULL);
    if (mu_online.resim) mu_online_abi_resim_phase(0);
    mu_online.resim = mu_online.console_active = mu_online.console_setup = 0;
    mu_online.rollback_active = mu_online.stable_rollback_active = 0;
    mu_gm_engine_state()->unk_C = 2;
}

static int console_inputs(int operation, int frame)
{
    if (!mu_console_abi(operation, (u32) frame, mu_online.last_local, mu_online.rxb,
                         NULL, NULL)) {
        console_fail(frame, "required input history unavailable");
        return 0;
    }
    mu_online.rxb_size = RXB_SIZE; /* console never consumes Slippi early-input extensions */
    return 1;
}

static int console_pad_renew(PADStatus* stat)
{
    int confirmed, frame = mu_online.frame;
    if (!mu_online.console_injecting) return 1; /* alarms never issue session frames */
    memset(stat, 0, 4 * sizeof *stat);
    wire_to_pad(&stat[mu_online.local_index], mu_online.last_local);
    confirmed = load_opponent_inputs(stat, frame, 1);
    record_used_pads(stat, frame, confirmed);
    mu_online.frame = frame + 1;
    return 0;
}

/* All bodies below this exclusive boundary were either authoritative when injected, or have
 * been compared by check_predictions(). During replay an unchecked prediction still blocks it,
 * even if CONSOLE_INPUTS/REPLAY has since received a contradicting authoritative pad. */
static int console_validate(int boundary)
{
    int r;
    for (r = 0; r < REMOTES; ++r) {
        if (mu_online.player_predicting[r] && mu_online.player_savestate_frame[r] < boundary)
            boundary = mu_online.player_savestate_frame[r];
    }
    if (!mu_console_abi(CONSOLE_VALIDATED, (u32) boundary, NULL, NULL, NULL, NULL)) {
        console_fail(boundary, "validated frontier unavailable");
        return 0;
    }
    return 1;
}

static void console_frame_begin(void)
{
    int frame = global_frame();
    void mu_console_replace_raw(void);
    void mu_console_rumble_step(void);
    if (mu_online.console_setup) {
        int local_port = 0;
        if (!mu_console_abi(CONSOLE_MATCH_BEGIN, 0, NULL, NULL,
                             &mu_online.console_epoch, &local_port)) {
            console_fail(0, "snapshot initialization unavailable");
            return;
        }
        mu_online.console_setup = 0;
        mu_online.console_active = 1;
        mu_online.local_index = (u8) local_port;
        /* The host routes console rumble by virtual session port, independently of hardware. */
        mu_online_audio_match_start(local_port, local_port);
        frame = global_frame(); /* unk_0, including pause, from the first actual VS body */
    }
    if (!mu_online.rollback_active) {
        /* Sample/send once, then compare only already executed bodies (seven RXB slots).
         * Admission waits before the oldest unresolved checkpoint could be evicted. */
        if (!console_inputs(CONSOLE_FORWARD, frame)) return;
        {
            int r;
            for (r = 0; r < REMOTES; ++r) {
                if (mu_online.player_predicting[r] &&
                    (int) be32(mu_online.rxb + RXB_OPNT_FRAME_NUMS + 4 * r) -
                        mu_online.player_savestate_frame[r] >= RXB_INPUTS_COUNT) {
                    console_fail(frame, "prediction history exceeded checkpoint horizon");
                    return;
                }
            }
        }
        check_predictions(frame);
        mu_online.stable_rollback_active = mu_online.rollback_active;
        mu_online.stable_rollback_end_frame = mu_online.rollback_end_frame;
        mu_online.stable_should_load = mu_online.rollback_should_load;
        mu_online.stable_savestate_frame = mu_online.savestate_frame;
    }
    if (mu_online.rollback_active) {
        if (mu_online.rollback_should_load) {
            const int ss = mu_online.savestate_frame;
            if (!load_state(ss)) {
                console_fail(ss, "required snapshot missing");
                return;
            }
            mu_online.resim = 1;
            mu_online_abi_resim_phase(1);
            frame = global_frame();
            mu_online_audio_console_restore(frame);
            mu_online.savestate_predicting = 0;
            mu_online.player_predicting[0] = mu_online.player_predicting[1] =
                mu_online.player_predicting[2] = 0;
            mu_online.rollback_should_load = mu_online.stable_should_load = 0;
        }
        if (!console_inputs(CONSOLE_REPLAY, frame)) return;
        ++mu_online.resim_frames;
    } else if (!console_inputs(CONSOLE_INPUTS, frame)) return;
    if (!console_validate(frame)) return;
    if (!mu_console_abi(CONSOLE_BOUNDARY, (u32) frame, NULL, NULL, NULL, NULL)) {
        console_fail(frame, "canonical boundary unavailable");
        return;
    }
    mu_online.frame = frame;
    mu_online.console_injecting = 1;
    mu_console_replace_raw(); /* historical injection, before capture and before queue consumption */
    mu_online.console_injecting = 0;
    /* Load frees the timeline; replay rebuilds these same seven retained checkpoints. */
    if (!capture_state(frame)) {
        console_fail(frame, "checkpoint capture failed");
        return;
    }
    mu_online_audio_frame_begin();
    /* Exactly once per simulated body, AFTER its checkpoint, on forward and replay alike. */
    mu_console_rumble_step();
}

static int console_body_finish(int current)
{
    mu_online_audio_frame_end(); /* includes the terminal body, even after unk_C is set */
    if (!mu_console_abi(CONSOLE_BODY_END, (u32) current, NULL, NULL, NULL, NULL)) {
        console_fail(current, "logical clock boundary unavailable");
        return 0;
    }
    return 1;
}

static void console_audio_finish(int terminal)
{
    mu_console_abi(CONSOLE_SERVICE, 1, NULL, NULL, NULL, NULL);
    mu_online_audio_rollback_end();
    if (terminal) mu_online_audio_match_exit();
    mu_console_abi(CONSOLE_SERVICE, 0, NULL, NULL, NULL, NULL);
}

static int console_frame_end(void)
{
    const int current = global_frame() - 1;
    if (!console_body_finish(current)) return ENGINE_CONTINUE;
    if (!mu_online.rollback_active) { console_audio_finish(0); return ENGINE_CONTINUE; }
    if (current < mu_online.rollback_end_frame) {
        camera_tasks(); /* camera/offscreen and fighter bone work needed by the next body */
        return ENGINE_RESIM;
    }
    mu_online.rollback_active = mu_online.stable_rollback_active = 0;
    mu_online.resim = 0;
    mu_online_abi_resim_phase(0);
    console_audio_finish(0);
    return ENGINE_ROLLBACK_DONE;
}

/* Called at the scene loop's pre-break site, after its counters advanced. Never tear down a
 * speculative scene. The native loop, scene objects and checkpoints remain alive while waiting. */
int mu_online_scene_terminal(void)
{
    int current;
    if (!mu_online.console_active) return 0; /* Slippi terminal path is unchanged */
    current = global_frame() - 1;
    if (!console_body_finish(current)) return 0;
    if (!console_inputs(CONSOLE_CONFIRM, current)) return 0;
    /* An earlier terminal body supersedes the previous replay frontier. Compare only the
     * predictions through this boundary; a correction restarts at its earliest checkpoint. */
    mu_online.rollback_active = mu_online.stable_rollback_active = 0;
    if (check_predictions(current)) {
        mu_online.stable_rollback_active = 1;
        mu_online.stable_rollback_end_frame = current;
        mu_online.stable_should_load = 1;
        mu_online.stable_savestate_frame = mu_online.savestate_frame;
        /* The loaded checkpoint will replace unk_C. The loop must re-enter before teardown. */
        return 1;
    }
    if (!console_validate(current + 1)) return 0;
    if (mu_online.resim) mu_online_abi_resim_phase(0);
    mu_online.resim = 0;
    mu_online_audio_console_commit(current); /* discard voices beyond an earlier corrected end */
    console_audio_finish(1);
    if (!mu_console_abi(CONSOLE_COMMIT, (u32) current, NULL, NULL, NULL, NULL)) {
        console_fail(current, "terminal commit unavailable");
        return 0;
    }
    logf_("console: rollback epoch %u committed body %d: %u loads, %u replay bodies",
          mu_online.console_epoch, current, mu_online.loads, mu_online.resim_frames);
    mu_online.console_active = mu_online.console_setup = 0;
    return 0; /* host is already lockstep; the next body uses session S+1 */
}

static void console_match_exit(void)
{
    /* A normal terminal commit already switched back to lockstep at the pre-break hook. */
    if (mu_online.console_active || mu_online.console_setup)
        console_fail(global_frame(), "scene left without terminal commitment");
}
