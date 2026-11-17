"""Pin the Mac scheduling gates, per-thread wiring, and append-only state format."""
from itertools import product
from pathlib import Path
import re
import subprocess
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text()


def function(text, name):
    start = re.search(r'\b' + name + r'\([^;{}]*\)\s*\{', text).end() - 1
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start + 1:end - 1]


class MacQosRoleWiringTest(unittest.TestCase):
    def setUp(self):
        self.embed = source('port/mac/embed_mac.mm')
        self.qos = source('port/mac/thread_qos.h')

    def test_focal_requires_on_screen_and_in_use(self):
        screen = function(self.embed, 'panel_on_screen')
        use = function(self.embed, 'panel_in_use')
        role = function(self.embed, 'keep_on_screen_role')
        self.assertIn('const bool on_screen = panel_on_screen(s);', role)
        self.assertIn('const bool in_use = panel_in_use(v);', role)
        self.assertIn('!releasing && in_use ? PRIO_DARWIN_ROLE_UI_FOCAL : PRIO_DARWIN_ROLE_UI_NON_FOCAL', role)
        self.assertIn('in_use ? v.role == desired : v.role != TASK_DARWINBG_APPLICATION', role)
        self.assertIn('in_use ? TASK_FOREGROUND_APPLICATION : TASK_BACKGROUND_APPLICATION', role)
        # Evaluate the actual return expressions over every visibility/focus combination.
        def expression(body):
            expr = re.search(r'return (.*?);', body, re.S)[1]
            expr = expr.replace('std::strcmp(v.front, "app") == 0', 'v.front == "app"')
            return ' '.join(re.sub(r'!(?!=)', 'not ', expr.replace('&&', ' and ').replace('||', ' or ')).split())
        for ordered, visible, mini, occlusion, active, key, front in product(
                (False, True), (False, True), (False, True), (0, 1),
                (False, True), (False, True), ('app', 'game', 'other')):
            s = SimpleNamespace(ordered=ordered, panel=SimpleNamespace(
                visible=visible, miniaturized=mini, occlusionState=occlusion))
            v = SimpleNamespace(active=active, key=key, visible=bool(occlusion), front=front)
            context = dict(s=s, v=v, NSWindowOcclusionStateVisible=1)
            actual = eval(expression(screen), {}, context) and eval(expression(use), {}, context)
            expected = ordered and visible and not mini and bool(occlusion) and (
                (active and key) or front == 'app')
            self.assertEqual(bool(actual), bool(expected), context)

    def test_release_is_owned_and_only_success_changes_ownership(self):
        role = function(self.embed, 'keep_on_screen_role')
        self.assertIn('bool role_promoted = false;', self.embed)
        self.assertIn('bool role_release_pending = false;', self.embed)
        self.assertIn('const bool releasing = s.role_promoted && s.role_release_pending;', role)
        success = re.search(r'if \(rc == 0\) \{([^}]+)\}', role)[1]
        self.assertIn('s.role_promoted = requested == PRIO_DARWIN_ROLE_UI_FOCAL;', success)
        self.assertIn('s.role_release_pending = false;', success)
        self.assertEqual(role.count('s.role_promoted ='), 1)
        self.assertEqual(role.count('s.role_release_pending = false;'), 1)
        self.assertIn('releasing ? (rc ? "release failed" : "released")', role)

    def test_hidden_or_unused_latches_release_before_any_early_gate(self):
        role = function(self.embed, 'keep_on_screen_role')
        latch = 'if (s.role_promoted && (!on_screen || !in_use)) s.role_release_pending = true;'
        self.assertIn(latch, role)
        self.assertLess(role.index(latch), role.index('if (!releasing && (!on_screen'))
        # Pin the actual latch predicate for every ownership/visibility/use combination.
        predicate = re.search(r'if \((.*?)\) s.role_release_pending = true;', role)[1]
        predicate = predicate.replace('&&', ' and ').replace('||', ' or ').replace('!', 'not ')
        for owned, on_screen, in_use in product((False, True), repeat=3):
            s = SimpleNamespace(role_promoted=owned)
            self.assertEqual(bool(eval(predicate, {}, dict(s=s, on_screen=on_screen, in_use=in_use))),
                             owned and (not on_screen or not in_use))
        self.assertIn('if (!releasing && (!on_screen || (in_use ? v.role == desired : '
                      'v.role != TASK_DARWINBG_APPLICATION))) return;', role)

    def test_rate_limit_retains_release_even_if_focus_returns(self):
        role = function(self.embed, 'keep_on_screen_role')
        limit = role.index('if (s.role_resets && now - s.role_asked_at < 1000) return;')
        self.assertLess(role.index('s.role_release_pending = true;'), limit)
        self.assertLess(limit, role.index('s.role_release_pending = false;'))
        self.assertNotIn('s.role_release_pending = false;', role[:limit])
        # Pending ownership alone forces the release, independently of the latest gates/role.
        predicate = re.search(r'const bool releasing = (.*?);', role)[1].replace('&&', ' and ')
        for owned, pending in product((False, True), repeat=2):
            s = SimpleNamespace(role_promoted=owned, role_release_pending=pending)
            self.assertEqual(bool(eval(predicate, {}, dict(s=s))), owned and pending)
        watch = function(self.embed, 'watch_state')
        self.assertLess(watch.index('keep_on_screen_role(s, v);'), watch.index('if (!reason && same) return;'))
        self.assertIn('now - s.state_at < 250', watch)
        self.assertIn('watch_state(s);', function(self.embed, 'pump'))

    def test_refusal_and_rate_limit_precede_each_attempt(self):
        role = function(self.embed, 'keep_on_screen_role')
        self.assertIn('if (s.role_refused) return;', role)
        self.assertIn('err == EPERM || err == ENOTSUP || err == EINVAL', role)
        self.assertIn('s.role_refused = true', role)
        self.assertLess(role.index('now - s.role_asked_at < 1000'), role.index('setpriority('))
        self.assertEqual(role.count('setpriority('), 1)
        self.assertEqual(role.count('host::log('), 1)
        self.assertIn('on-screen role %s -> %s;', role)
        self.assertIn('role_name(v.role), role_name(current_role(TASK_UNSPECIFIED))', role)
        self.assertIn('#define PRIO_DARWIN_ROLE_UI_FOCAL 1', self.embed)
        self.assertIn('#define PRIO_DARWIN_ROLE_UI_NON_FOCAL 4', self.embed)

    def test_fast_and_regular_watchers_use_the_same_gate(self):
        self.assertIn('keep_on_screen_role(s, v);', function(self.embed, 'watch_state'))
        self.assertIn('keep_on_screen_role(s, v);', function(self.embed, 'pump'))
        self.assertEqual(self.embed.count('setpriority('), 1)

    def test_qos_is_once_per_actual_thread_and_reports_return_code(self):
        self.assertIn('thread_local const bool requested', self.qos)
        self.assertEqual(self.qos.count('pthread_set_qos_class_self_np('), 2)
        self.assertIn('pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0)', self.qos)
        self.assertIn('name, rc, rc ? std::strerror(rc) : "ok", current_thread_qos_name()', self.qos)
        self.assertIn('pthread_get_qos_class_np(pthread_self(), &qos, nullptr)', self.qos)
        self.assertIn('case QOS_CLASS_UNSPECIFIED: return "unset";', self.qos)

    def test_audio_publishes_without_logging_and_main_pump_reports_once(self):
        audio = source('port/linux/full/audio_sdl.cpp')
        request = function(self.qos, 'audio_thread_qos_once')
        self.assertIn('thread_local bool requested = false;', request)
        self.assertIn('if (requested) return;', request)
        self.assertLess(request.index('requested = true;'), request.index('pthread_set_qos_class_self_np('))
        self.assertIn('pthread_get_qos_class_np(pthread_self(), &qos, nullptr)', request)
        self.assertIn('audio_qos_result.store(result, std::memory_order_relaxed);', request)
        self.assertIn('std::atomic<uint64_t>::is_always_lock_free', self.qos)
        # Whitelist the QoS helper's whole call graph: additions need a real-time review.
        calls = set(re.findall(r'\b([\w:]+)\s*\(', request))
        self.assertEqual(calls, {'if', 'pthread_set_qos_class_self_np', 'pthread_get_qos_class_np',
                                 'pthread_self', 'uint64_t', 'uint32_t', 'store'})
        jukebox = source('port/runtime/hle/jukebox.cpp')
        callback_path = [function(audio, name) for name in
                         ('callback', 'render', 'mix_ui_sound', 'output_volume')]
        callback_path += [request, function(jukebox, 'mix'), function(jukebox, 'render')]
        for body in callback_path:
            self.assertNotRegex(body, r'\b(?:host::)?log\s*\(')
            self.assertNotIn('interactive_thread_qos_once(', body)
            self.assertNotIn('report_audio_thread_qos(', body)
        report = function(self.qos, 'report_audio_thread_qos')
        self.assertIn('audio_qos_result.exchange(audio_qos_empty, std::memory_order_relaxed)', report)
        self.assertIn('if (result == audio_qos_empty) return;', report)
        self.assertEqual(report.count('host::log('), 1)
        pump = function(self.embed, 'pump')
        self.assertLess(pump.index('mac_host::report_audio_thread_qos();'), pump.index('if (!surface)'))
        close = function(audio, 'audio_close')
        self.assertLess(close.index('SDL_CloseAudioDevice('), close.index('mac_host::report_audio_thread_qos();'))

    def test_calls_run_inside_each_thread_in_both_launch_modes(self):
        main = source('port/app/main.cpp')
        render = source('port/runtime/gx/threaded_backend.cpp')
        audio = source('port/linux/full/audio_sdl.cpp')
        sim_call = 'mac_host::interactive_thread_qos_once("simulation");'
        self.assertLess(main.index('host::options.log_file = argv[++i]'), main.index(sim_call))
        self.assertLess(main.index(sim_call), main.index('backend = gx::create_threaded_backend'))
        self.assertRegex(render, r'worker = std::thread\([^\n]+\{\n#ifdef __APPLE__\n'
                         r' +mac_host::interactive_thread_qos_once\("render/submit"\);')
        self.assertRegex(audio, r'void callback\([^\n]+\{\n#ifdef __APPLE__\n'
                         r' +mac_host::audio_thread_qos_once\(\);')
        self.assertIn('want.callback = callback;', audio)
        self.assertIn('SDL_OpenAudioDevice(', audio)
        self.assertNotIn('yougame::active()', self.qos)

    def test_all_qos_code_disappears_from_non_apple_projections(self):
        for path in ('port/app/main.cpp', 'port/runtime/gx/threaded_backend.cpp',
                     'port/linux/full/audio_sdl.cpp', 'port/mac/thread_qos.h'):
            with self.subTest(path=path):
                p = subprocess.Popen(['/usr/bin/unifdef', '-U__APPLE__', str(ROOT / path)],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                out, err = p.communicate(timeout=5)
                self.assertIn(p.returncode, (0, 1), err)
                self.assertNotIn(b'thread_qos.h', out)
                self.assertNotIn(b'interactive_thread_qos_once', out)
                self.assertNotIn(b'audio_thread_qos', out)
                self.assertNotIn(b'pthread_set_qos_class', out)
                self.assertNotIn(b'QOS_CLASS_', out)

    def test_state_format_only_appends_qos_after_all_existing_fields(self):
        watch = function(self.embed, 'watch_state')
        formats = re.search(r'host::log\(((?:"[^"\n]*"\s*)+), wall,', watch)[1]
        actual = ''.join(re.findall(r'"([^"\n]*)"', formats))
        old = ('embed: state t=%lld (%s): app %s, panel %s, %s, front %s, role %s, '
               '%d threads at priority %d-%d, activity %s%s')
        self.assertEqual(actual, old + ', main QoS %s')
        self.assertIn('v.held ? "held" : "none", repeated, v.main_qos', watch)
        self.assertIn('out.main_qos = mac_host::current_thread_qos_name();', self.embed)
        self.assertIn('v.max_priority <= 4, v.main_qos', watch)


if __name__ == '__main__':
    unittest.main()
