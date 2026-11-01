"""Host/game isolation boundaries which the window-free engine fake cannot execute."""
from pathlib import Path
import re
import subprocess
import shutil
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]


def read(path):
    return (ROOT / path).read_text()


def body(text, name):
    # Balanced braces, excluding comments and strings from the brace scan.
    mask = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/',
                  lambda m: ' ' * len(m[0]), text, flags=re.S)
    match = re.search(r'\b' + name + r'\([^;]*?\)\s*\{', mask)
    if not match:
        raise AssertionError('Missing function: ' + name)
    depth, at = 1, match.end()
    while depth:
        depth += (mask[at] == '{') - (mask[at] == '}')
        at += 1
    return text[match.end():at - 1]


class ConsoleWiring(unittest.TestCase):
    def setUp(self):
        self.host = read('port/app/source_host.cpp')

    def test_boot_intake_precedes_source_run_and_uses_window_only_pump(self):
        main = read('port/app/main.cpp')
        at = main.index('yougame::console::boot(')
        self.assertLess(main.index('yougame::wait_identity(10000)'), at)
        self.assertLess(main.index('yougame::console::take_pending_launch()'), at)
        self.assertLess(at, main.index('source_port::run(shutdown)'))
        boot = main[at:main.index('source_port::run(shutdown)')]
        for text in ('take_request()', 'start_session(q, c, e)', 'console_begin(std::move(link), c)',
                     'console_boot_ready()', 'host::window_pump()', 'screen_label::show'):
            self.assertIn(text, boot)
        self.assertNotIn('host::retrace()', boot)
        helper = read('port/runtime/hle/yougame_console_boot.h')
        self.assertLess(helper.index('ready();'), helper.index('auto link = responsive_wait('))

    def test_app_route_caps_and_rejoin_metadata_reach_console(self):
        link = read('port/runtime/hle/yougame_link.cpp')
        self.assertIn('console::direct_notice(push)', link)
        self.assertIn('g_relaunch = console::relaunch_cap(j)', link)
        self.assertIn('g_relaunch = console::relaunch_cap(hello)', link)
        self.assertIn('broker_->open(ticket, room, timeout, session)', link)
        stream = read('port/runtime/hle/yougame_transport.h')
        self.assertIn('if (!session.empty()) q["session"] = session;', stream)

    def test_links_are_serviced_during_boot_and_loading_and_relaunch_has_no_leave(self):
        link = read('port/runtime/hle/yougame_console_link.cpp')
        production = read('port/runtime/hle/yougame_link.cpp')
        self.assertIn('io.background_service = true;', production)
        self.assertIn('link->service(); return link;', link)
        service = body(link, 'service')
        self.assertIn('pump(0)', service)
        for forbidden in ('retrace(', 'advance(', 'poll('):
            self.assertNotIn(forbidden, service)
        engine = read('port/runtime/hle/yougame_console_engine.cpp')
        self.assertIn('link_->pump(0)', body(engine, 'poll'))
        backend = production[production.index('struct ConsoleBackend'):production.index('SessionIO console_io(')]
        self.assertIn('room->close(1000)', backend)
        self.assertNotIn('"leave"', backend)
        self.assertIn('broker_->close(1000, code)', production)
        entry = body(production, 'friends_entry')
        self.assertNotIn('"leave"', entry)
        self.assertNotIn('close_room(', entry)
        self.assertIn('str(m,"sessionId") == prepared_session', entry)
        self.assertIn('io.close = [] {};', entry)
        main = read('port/app/main.cpp')
        shutdown = main[main.index('  auto shutdown = [](int) {'):]
        self.assertLess(shutdown.index('console_engine().end("window closed")'),
                        shutdown.index('backend_at_exit->reset()'))
        self.assertIn('b->session = friends::text(request,"sessionId",64);', production)
        self.assertIn('open_session("",ticket,code,&error,ms,session)', backend)

    def test_main_build_friends_default_keeps_console_handoff(self):
        self.assertIn('#define YOUGAME_FRIENDS_ROW 1', read('port/runtime/hle/yougame_mode_policy.h'))
        practice = body(read('port/runtime/hle/native_practice.cpp'), 'tick')
        self.assertIn('yougame::console::friends_entry(room);', practice)
        self.assertIn('yougame::friends::events.take_host()', practice)
        events = read('port/runtime/hle/yougame_friends.h')
        self.assertIn('friend_entry_ = visible && selected;', events)

    def test_body_boundary_replaces_queue_before_game_consumes_it(self):
        # mu_pad.c is the host side (mu_shim.h); mu_console_pad.c the game side (controller/VI headers):
        # the game's C library and the host's stdint collide (ssize_t) in one file.
        pad = read('sourceport/game/shim/mu_pad.c')
        game = read('sourceport/game/shim/mu_console_pad.c')
        self.assertNotIn('#include "mu_shim.h"', game)
        self.assertNotIn('controller.h', pad)
        self.assertIn('return mu_host->console_frame_begin && mu_host->console_frame_begin();',
                      body(pad, 'mu_console_host_frame_begin'))
        hook = body(game, 'mu_console_frame_begin')
        self.assertLess(hook.index('mu_console_host_frame_begin()'), hook.index('mu_console_replace_raw()'))
        replace = body(game, 'mu_console_replace_raw')
        self.assertIn('p->qread = p->qwrite;', replace)
        self.assertIn('p->qcount = 0;', replace)
        self.assertIn('HSD_PadRenewRawStatus(0)', replace)
        self.assertIn('mu_console_frame_begin();',
                      body(read('sourceport/game/shim/mu_lab.c'), 'mu_te_frame_begin'))
        patch = read('sourceport/patches/melee-native.patch')
        start = patch.index('+            mu_te_frame_begin();')
        self.assertLess(start, patch.index('             lb_800198E0();', start))
        self.assertLess(start, patch.index('if (gm_801A46B8(0) || !gm_GetDbPauseFlag(0))', start))
        self.assertIn('if (!g_console_boot) return 0;', body(self.host, 'h_console_frame_begin'))

    def test_scene_gate_requests_one_retrace_before_one_body(self):
        gate = body(read('sourceport/game/shim/mu_online.c'), 'mu_online_engine_gate')
        self.assertLess(gate.index('mu_console_engine_gate(count)'), gate.index('if (!hooks_on())'))
        console = body(read('sourceport/game/shim/mu_console_pad.c'), 'mu_console_engine_gate')
        self.assertRegex(console, r'if \(!mu_console_active\(\)\)\s*return 0;')
        self.assertLess(console.index('return 0;'), console.index('HSD_PadLibData.qcount = 0'))
        self.assertLess(console.index('VIWaitForRetrace()'), console.index('*count = 1'))
        self.assertIn('VIGetRetraceCount() == body_retrace', console)
        self.assertIn('mu_console_replace_raw()', console)  # fallback when no pad alarm is armed
        patch = read('sourceport/patches/melee-native.patch')
        at = patch.index('+            pad_queue_count = lb_80019894();')
        self.assertLess(patch.index('mu_online_engine_gate(&pad_queue_count)', at),
                        patch.index('for (i = 0; i < pad_queue_count; i++)', at))

    def test_hardware_bypasses_local_game_mutators(self):
        sample = body(self.host, 'console_hardware')
        self.assertIn('host::input_read_console_devices(raw)', sample)
        self.assertNotIn('input_poll(', sample)
        devices = body(read('port/runtime/host/window.cpp'), 'input_read_console_devices')
        self.assertIn('out[i].err = -1;', devices)
        self.assertIn('debug.feeding[i].kind != DeviceKind::Keyboard', devices)
        self.assertIn('if (controller)', devices)
        pad = body(self.host, 'h_pad_read')
        boundary = pad[:pad.index('if (g_online_test_mode == 0)')]
        self.assertIn('if (g_console_boot)', boundary)
        self.assertIn('engine.read(engine.active() ? yougame::console::Pads{} : console_hardware()', boundary)
        self.assertIn('return;', boundary)
        for mutator in ('user_gecko::apply()', 'offline_delay::apply(', 'lcancel::apply('):
            self.assertNotIn(mutator, boundary)

    def test_clock_and_seed_are_session_gated(self):
        for name in ('h_ticks', 'h_boot_time', 'h_vi_retrace_count', 'h_vi_next_field'):
            self.assertIn('if (yougame::console::console_active())', body(self.host, name))
        ticks = body(self.host, 'h_ticks')
        self.assertIn('return host::cpu->tb;', ticks)
        self.assertIn('return host::console_epoch_ticks();', body(self.host, 'h_boot_time'))
        poll = body(self.host, 'h_poll')
        console = poll[poll.index('if (yougame::console::console_active())'):]
        console = console[:console.index('return;')]
        self.assertIn('engine.poll()', console)
        self.assertIn('engine.retrace_due() && !g_console_in_retrace', console)
        self.assertIn('fire_alarms(engine.clock_ticks())', console)
        self.assertNotIn('host::retrace_due()', console)
        retrace = body(self.host, 'native_retrace')
        self.assertLess(retrace.index('engine.service_retrace()'), retrace.index('fire_alarms(engine.alarm_ticks())'))
        self.assertLess(retrace.index('fire_alarms(engine.alarm_ticks())'), retrace.index('g_game.retrace()'))
        self.assertIn('retrace_count()', body(self.host, 'h_vi_retrace_count'))
        self.assertIn('retrace_count() & 1', body(self.host, 'h_vi_next_field'))
        entry = read('sourceport/game/shim/mu_entry.c')
        self.assertLess(body(entry, 'mu_run').index('mu_apply_seed(seed)'),
                        body(entry, 'mu_run').index('return main()'))
        pad = read('sourceport/game/shim/mu_pad.c')
        self.assertIn('!seeded && mu_host->console_seed', body(pad, 'mu_console_host_frame_begin'))

    def test_card_cannot_import_or_prepare_players_save(self):
        self.assertIn('if (yougame::console::console_active()) return;', body(self.host, 'apply_detected_mods'))
        self.assertIn('if (!g_console_boot) {\n    const fs::path ordinary', self.host)
        mount = body(self.host, 'card_mount_files')
        self.assertIn('g_console_boot ? g_console_card', mount)
        self.assertIn('if (g_console_boot) break;', mount)
        self.assertIn('if (!g_console_boot) host::backup_card_folder', mount)
        begin = body(self.host, 'begin_console')
        self.assertIn('g_game.run', begin)  # reject a late entry after game initialization
        self.assertIn('prepare_card(', begin)

    def test_settings_and_page_cannot_mutate_console_simulation(self):
        for name in ('h_game_options', 'h_game_options2', 'h_game_options3',
                     'h_audio_options', 'h_display_options', 'h_mod_flags', 'h_match_override',
                     'h_rng_seed_override', 'h_online_test_match'):
            self.assertIn('if (yougame::console::console_active())', body(self.host, name))
        self.assertIn('if (yougame::console::console_active()) return;',
                      body(read('port/runtime/hle/native_practice.cpp'), 'tick'))
        bridge = body(self.host, 'console_practice_bridge')
        self.assertIn('args[0] == -1', bridge)  # only query may reach page pause
        self.assertIn('return -1;', bridge)

    def test_notice_pump_and_snapshot_coverage(self):
        begin = body(self.host, 'begin_console')
        self.assertIn('host::window_pump();', begin)
        self.assertIn('screen_label::show(text, 8.0)', begin)
        self.assertNotIn('host::retrace()', re.sub(r'//[^\n]*', '', begin))
        snap = body(read('sourceport/game/shim/mu_snapshot.c'), 'mu_state_snapshot_words')
        for field in ('HSD_RandSeedPtr', 'gm_GetCurrentSceneIndex()', 'fp->cur_pos.x',
                      'mn_804A04F0.cur_menu', 'mn_804A04F0.hovered_selection',
                      'state->match_result', 'state->timer_seconds', 'rules->item_freq',
                      'rules->stkind', 'Player_GetDamage(slot)'):
            self.assertIn(field, snap)

    def test_console_rollback_is_explicit_and_preserves_slippi_setup(self):
        begin = body(self.host, 'begin_console')
        self.assertIn('std::getenv("MELEE_CONSOLE_ROLLBACK")', begin)
        self.assertIn('rollback_setting(std::getenv("MELEE_CONSOLE_ROLLBACK"))', begin)
        self.assertIn('config, std::move(hooks), rollback)', begin)
        native = read('sourceport/game/shim/mu_online.c')
        self.assertEqual(body(native, 'mu_online_active').strip(), 'return mu_online.active;')
        self.assertEqual(body(native, 'mu_online_pending').strip(), 'return mu_online.pending;')
        self.assertIn('mu_online.console_active ? mu_gm_engine_state()->unk_0 : mu_gm_engine_state()->unk_8',
                      body(native, 'global_frame'))
        for name in ('mu_online_start_melee', 'mu_online_arm', 'mu_online_codes'):
            self.assertNotIn('mu_online.console_', body(native, name))
            self.assertNotIn('mu_console_', body(native, name))
        console = read('sourceport/game/shim/mu_console_rollback.inc')
        for forbidden in ('sync_rng_proc(', 'mu_replay_online_start(', 'report_game(',
                          'mu_online_rules_set(', 'CMD_GET_MATCH_STATE', 'HSD_RandSeedPtr'):
            self.assertNotIn(forbidden, console)
        codes = body(console, 'mu_console_rollback_codes')
        for rule in ('PREVENT_WOBBLING', 'PS_FROZEN_CHECK'):
            self.assertNotIn(rule, codes)
        setup = body(console, 'mu_console_match_setup')
        self.assertIn('GS_VS', setup)
        self.assertIn('GS_SUDDEN_DEATH', setup)
        patch = read('sourceport/patches/melee-native.patch')
        at = patch.index('+    mu_console_match_setup();')
        self.assertLess(at, patch.index('     fn_8016DCC0(arg0);', at))

    def test_console_replay_admission_capture_and_failure_order(self):
        console = read('sourceport/game/shim/mu_console_rollback.inc')
        begin = body(console, 'console_frame_begin')
        self.assertLess(begin.index('CONSOLE_FORWARD'), begin.index('check_predictions(frame)'))
        self.assertLess(begin.index('load_state(ss)'), begin.index('CONSOLE_REPLAY'))
        self.assertLess(begin.index('CONSOLE_REPLAY'), begin.index('mu_console_replace_raw()'))
        self.assertLess(begin.index('mu_console_replace_raw()'), begin.index('capture_state(frame)'))
        shim = body(read('sourceport/game/shim/mu_console_pad.c'), 'mu_console_frame_begin')
        self.assertLess(shim.index('mu_console_rollback_active()'), shim.index('mu_console_host_frame_begin()'))
        host = body(self.host, 'h_console_rollback')
        self.assertIn('begin_native_savestates()', host)
        replay = host[host.index('case MU_CONSOLE_REPLAY:'):host.index('case MU_CONSOLE_INPUTS:')]
        self.assertIn('engine.replay(body)', replay)
        for forbidden in ('advance(', 'console_hardware(', 'retrace(', 'send_pad('):
            self.assertNotIn(forbidden, replay)
        commands = body(self.host, 'h_slippi_command')
        before_slippi = commands[:commands.index('return slippi::online::native_command(')]
        self.assertIn('!g_savestates.has(frame)', before_slippi)
        self.assertIn('required snapshot missing: epoch %u body %d', before_slippi)
        self.assertIn('return -1;', before_slippi)
        self.assertNotIn('abort(', before_slippi)
        self.assertNotIn('die(', before_slippi)


    def test_terminal_commit_precedes_scene_break_and_lockstep_teardown(self):
        patch = read('sourceport/patches/melee-native.patch')
        at = patch.index('+                if (mu_online_scene_terminal())')
        self.assertLess(at, patch.index('                 break;', at))
        self.assertIn('goto mu_online_body;', patch[at:at + 160])
        console = read('sourceport/game/shim/mu_console_rollback.inc')
        terminal = body(console, 'mu_online_scene_terminal')
        for first, second in [('console_body_finish(current)', 'CONSOLE_CONFIRM'),
                              ('CONSOLE_CONFIRM', 'check_predictions(current)'),
                              ('mu_online_abi_resim_phase(0)', 'CONSOLE_COMMIT'),
                              ('console_audio_finish(1)', 'CONSOLE_COMMIT')]:
            self.assertLess(terminal.index(first), terminal.index(second))
        self.assertIn('return 1;', terminal)  # correction re-enters the retained scene loop
        self.assertIn('scene left without terminal commitment', body(console, 'console_match_exit'))
        host = body(self.host, 'h_console_rollback')
        self.assertIn('engine.commit_match(body, state)', host)

    def test_replay_clock_and_hardware_alarm_domains(self):
        poll = body(self.host, 'h_poll')
        self.assertLess(poll.index('engine.poll()'), poll.index('if (g_resim)'))
        self.assertIn('if (engine.body_active() && !engine.servicing()) return;', poll)
        self.assertIn('ConsoleServiceScope service;', poll)
        self.assertIn('engine.logical_retrace()', poll)
        commands = body(self.host, 'h_slippi_command')
        self.assertIn('console.capture_clock(frame)', commands)
        self.assertIn('console.restore_clock(frame)', commands)
        alarm = read('sourceport/game/shim/mu_os.c')
        for name in ('OSSetAlarm', 'OSSetAbsAlarm', 'OSSetPeriodicAlarm'):
            self.assertIn('console_alarm_clock(&simulation, &hardware)', body(alarm, name))
        load = body(read('sourceport/game/shim/mu_online.c'), 'load_state')
        self.assertLess(load.index('mu_alarms_hold()'), load.index('command('))
        self.assertLess(load.index('command('), load.index('mu_alarms_release()'))

    def test_presentation_and_diagnostics_keep_console_identity(self):
        audio = read('sourceport/game/shim/mu_online_audio.c')
        self.assertIn('mu_gm_engine_state()->unk_0', body(audio, 'engine_frame'))
        for name in ('sfx_hooks_on', 'mu_online_audio_frame_begin', 'mu_online_audio_frame_end',
                     'mu_online_music_defer', 'mu_online_motor_port', 'mu_online_audio_rollback_end',
                     'mu_online_audio_match_exit'):
            self.assertIn('mu_console_rollback_active()', body(audio, name))
        console = read('sourceport/game/shim/mu_console_rollback.inc')
        self.assertIn('mu_online_audio_match_start(local_port, local_port)', body(console, 'console_frame_begin'))
        self.assertIn('camera_tasks()', body(console, 'console_frame_end'))
        self.assertIn('gx::mark_discontinuity(true)', body(self.host, 'native_load'))
        self.assertIn('if (!yougame::console::console_engine().match_active()) slippi::online::note_rollback(frame)',
                      body(self.host, 'native_load'))


GIT = '/Library/Developer/CommandLineTools/usr/bin/git' if sys.platform == 'darwin' else (shutil.which('git') or 'git')
BASE = 'eb04f3c'
# The Ranked merge (yg-ranked-port) changed these Slippi routines; they are pinned to its tip instead.
RANKED_BASE = '9152358'
RANKED_ROUTINES = ('mu_online_start_melee', 'report_game')


def baseline(path, base=BASE):
    if not (ROOT / '.git').exists():
        raise unittest.SkipTest('history checks require a git checkout')
    available = subprocess.run([GIT, 'cat-file', '-e', base + '^{commit}'], cwd=ROOT,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if available.returncode:
        raise unittest.SkipTest('history checks require revision ' + base)
    return subprocess.check_output([GIT, 'show', base + ':' + path], cwd=ROOT).decode().replace('\r\n', '\n')


def tokens(text):
    text = re.sub(r'//[^\n]*|/\*.*?\*/', '', text, flags=re.S)
    return re.findall(r'[A-Za-z_][A-Za-z_0-9]*|[0-9]+|[^\s]', text)


def functions(text):
    return re.findall(r'(?m)^(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w* ]*?\b(\w+)\([^;{}]*\)\s*\{', text)


def drop_block(text, condition):
    # Remove only a specifically named console-only if, with balanced braces.
    start = text.index(condition)
    brace = text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[:start] + text[end:]


class ConsoleOffEquivalence(unittest.TestCase):
    def assert_functions(self, path, specialize):
        old, new = baseline(path), specialize(read(path))
        names = functions(old)
        self.assertGreater(len(names), 1)
        for name in names:
            with self.subTest(path=path, function=name):
                self.assertEqual(tokens(body(new, name)), tokens(body(old, name)))

    def test_slippi_audio_functions_equal_pre_rollback_when_console_false(self):
        def off(text):
            for old, new in (
                ('(mu_console_rollback_active() ? mu_gm_engine_state()->unk_0 :\n'
                 '                                                 mu_gm_engine_state()->unk_8)', 'mu_gm_engine_state()->unk_8'),
                ('(mu_online_active() || mu_console_rollback_active())', 'mu_online_active()'),
                ('(mu_gm_engine_state()->unk_C == 0 || mu_console_rollback_active())', 'mu_gm_engine_state()->unk_C == 0'),
                ('(mu_gm_engine_state()->unk_C != 0 && !mu_console_rollback_active())', 'mu_gm_engine_state()->unk_C != 0'),
                ('(!mu_online_resim_active() && !(mu_console_rollback_active() && audio.in_body))', '!mu_online_resim_active()'),
            ):
                self.assertIn(old, text)
                text = text.replace(old, new)
            return drop_block(text, 'if (mu_console_rollback_active()) {')
        self.assert_functions('sourceport/game/shim/mu_online_audio.c', off)

    def test_os_alarm_and_callback_functions_equal_pre_rollback_when_console_false(self):
        def off(text):
            text = text.replace('    int console_clock;\n', '')
            text = re.sub(r'    console_clock = mu_host->version >= 25 && mu_host->console_rollback &&\s*'
                          r'mu_host->console_rollback\(MU_CONSOLE_SERVICE, 1, NULL\);', '', text)
            text = text.replace('    if (console_clock) mu_host->console_rollback(MU_CONSOLE_SERVICE, 0, NULL);', '')
            for _ in range(2):
                text = drop_block(text, 'if (mu_host->version >= 25 && mu_host->console_alarm_clock &&')
            text = text.replace('    uint64_t simulation, hardware;\n', '')
            text = re.sub(r'    int console = mu_host->version >= 25 && mu_host->console_alarm_clock &&\s*'
                          r'mu_host->console_alarm_clock\(&simulation, &hardware\);', '', text)
            text = text.replace('console ? (OSTime) hardware : (OSTime) mu_host->ticks()', '(OSTime) mu_host->ticks()')
            text = text.replace('    if (console) start += (OSTime) hardware - (OSTime) simulation;', '')
            self.assertNotIn('console_clock', text)
            self.assertNotIn('console_alarm_clock', text)
            return text
        self.assert_functions('sourceport/game/shim/mu_os.c', off)

    def test_snapshot_prefix_and_all_nonconsole_functions_equal_pre_rollback(self):
        def off(text):
            text = drop_block(text, 'if (mu_console_rollback_active()) {')
            # The enlarged zeroed suffix has console_valid=0. The first 80 words are identical.
            self.assertIn('__builtin_memset(out, 0, 103 * sizeof *out);', text)
            return text.replace('103 * sizeof *out', '(2 + 6 * 12 + 2 + 4) * sizeof *out')
        self.assert_functions('sourceport/game/shim/mu_snapshot.c', off)

    def test_entry_only_changes_pod_size_assertion_and_preserves_field_offsets(self):
        path = 'sourceport/game/shim/mu_entry.c'
        self.assertEqual(read(path).replace('103 * sizeof(uint32_t)', '80 * sizeof(uint32_t)'), baseline(path))
        path = 'port/runtime/abi/mu_host.h'
        old = re.search(r'typedef struct MuStatePod \{(.*?)\} MuStatePod;', baseline(path), re.S)[1]
        new = re.search(r'typedef struct MuStatePod \{(.*?)\} MuStatePod;', read(path), re.S)[1]
        legacy = tokens(old)
        self.assertEqual(tokens(new)[:len(legacy)], legacy)
        self.assertEqual(body(read('port/runtime/hle/yougame_console_engine.cpp'), 'state_digest').count(
            'offsetof(MuStatePod, console_valid)'), 2)

    def test_original_online_bodies_remain_identical(self):
        path = 'sourceport/game/shim/mu_online.c'
        old, new = baseline(path), read(path)
        # Round A moved the predictor verbatim into an include, preserving its OFF branches.
        new += read('sourceport/game/shim/mu_online_prediction.inc')
        # Round A's explicit console branches are already independently pinned above; these
        # pin the untouched Slippi routines, including extracted predictor helpers.
        names = (
            'axis_crossed be32 camera_tasks clamp_stick_at_rest command command_into compute_checksum '
            'correction_capture correction_measure describe_address direct_renew early_contradicts '
            'early_pad early_rb_any early_rollback_on early_trace_on end_game fetch_match_state '
            'find_in_jobj_tree float_bits hud_message inside jobj_field launch_kind logf_ mu_online_active '
            'mu_online_after_pad_renew mu_online_arm mu_online_codes mu_online_is_test_run '
            'mu_online_match_exit mu_online_p2p_menus mu_online_p2p_port mu_online_pending '
            'mu_online_pending_game_info mu_online_record_state mu_online_reset_game_index '
            'mu_online_resim_active mu_online_start_melee mu_online_state_address mu_online_state_size '
            'mu_online_test_wait pad_to_wire pads_contradict_zone put32 record_used_pads report_game '
            'selftest_attribute selftest_command side_of singles_winner store_recovery sync_rng_proc '
            'test_inputs_enabled test_pad trace_pads trace_pads_line trace_renew trace_state_line used_slot '
            'wire_to_pad '
        ).split()
        self.assertEqual(len(names), 61)
        ranked = baseline(path, RANKED_BASE)
        for name in names:
            self.assertEqual(body(new, name), body(ranked if name in RANKED_ROUTINES else old, name))

    def test_frontier_and_rumble_ordering_and_alarm_guard(self):
        console = read('sourceport/game/shim/mu_console_rollback.inc')
        begin = body(console, 'console_frame_begin')
        self.assertLess(begin.index('check_predictions(frame)'), begin.index('console_validate(frame)'))
        self.assertLess(begin.index('console_validate(frame)'), begin.index('CONSOLE_BOUNDARY'))
        self.assertLess(begin.index('capture_state(frame)'), begin.index('mu_console_rumble_step()'))
        terminal = body(console, 'mu_online_scene_terminal')
        self.assertLess(terminal.index('check_predictions(current)'), terminal.index('console_validate(current + 1)'))
        engine = read('port/runtime/hle/yougame_console_engine.cpp')
        self.assertIn('it->first > validated_boundary_', body(engine, 'publish'))
        self.assertNotIn('confirmed_through()', body(engine, 'publish'))
        self.assertNotIn('validated_boundary_', body(engine, 'receive'))
        game = read('sourceport/game/shim/mu_console_pad.c')
        replace = body(game, 'mu_console_replace_raw')
        rollback = replace[:replace.index('    p->qread = p->qwrite;\n    p->qcount = 0;')]
        self.assertNotIn('HSD_PadRenewRawStatus', rollback)
        self.assertNotIn('HSD_PadRumbleInterpret', rollback)
        self.assertIn('mu_console_rumble_owned()', body(game, 'mu_console_engine_gate'))
        self.assertEqual(body(console, 'mu_console_rumble_owned').strip(),
                         'return mu_online.console_setup || mu_online.console_active;')
        patch = read('sourceport/patches/melee-native.patch')
        self.assertIn('+    if (!mu_console_rumble_owned())\n+#endif\n     HSD_PadRumbleInterpret();', patch)

    def test_sdk_renewal_off_keeps_pre_rollback_text_and_order(self):
        path = 'sourceport/patches/melee-native.patch'
        def controller(text):
            start = text.index('diff --git a/src/sysdolphin/baselib/controller.c ')
            end = text.index('\ndiff --git ', start + 1)
            return re.sub(r'^@@[^\n]*\n', '', text[start:end], flags=re.M)
        now = controller(read(path))
        added = ('+#ifdef MU_NATIVE\n'
                 '+    /* Console rollback steps scripts once per body, after its checkpoint, never on alarms. */\n'
                 '+    if (!mu_console_rumble_owned())\n+#endif\n')
        self.assertEqual(now.count(added), 1)
        self.assertEqual(now.replace(added, ''), controller(baseline(path)))

    def test_patch_hunk_counts(self):
        patch = read('sourceport/patches/melee-native.patch')
        hunks = list(re.finditer(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@[^\n]*\n', patch, re.M))
        # Build 28 plus upstream v0.9.86 has 10067 hunks.
        self.assertEqual(len(hunks), 10067)
        for i, h in enumerate(hunks):
            after = patch[h.end():hunks[i + 1].start() if i + 1 < len(hunks) else len(patch)]
            after = after.split('diff --git ')[0]
            lines = after.splitlines()
            old = sum(line.startswith((' ', '-')) for line in lines)
            new = sum(line.startswith((' ', '+')) for line in lines)
            self.assertEqual((old, new), (int(h[2] or 1), int(h[4] or 1)), h[0])


if __name__ == '__main__':
    unittest.main()
