"""Source regressions for the v0.9.65 merge; no game, network or Windows build.

Existing listing, results, loader, focus and agent-hook tests remain unchanged.
"""
from pathlib import Path
import re
import unittest

from relay_pads_wiring_test import code, function

ROOT = Path(__file__).resolve().parents[2]
CONFLICTED = (
    "port/CMakeLists.txt", "port/runtime/hle/slippi_online.cpp",
    "port/runtime/host/controller_profiles.cpp", "port/runtime/host/controller_profiles.h",
    "port/runtime/host/net_trace_file.cpp", "port/runtime/host/window.cpp",
)


def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


class YouGame0965MergeTest(unittest.TestCase):
    def test_merge_boundaries_have_no_markers(self):
        for path in CONFLICTED:
            with self.subTest(path=path):
                self.assertIsNone(re.search(r"^(?:<{7}|={7}|>{7})(?: |$)", source(path), re.M))

    def test_rematch_version_check_preserves_yougame_and_local_peers(self):
        online = code(source("port/runtime/hle/slippi_online.cpp"))
        prepare = function(online, "prepare_online_match_state")
        gate = re.search(r"if\s*\(([^{};]*?)\)\s*\{\s*std::string version_error;", prepare)
        self.assertIsNotNone(gate)
        for condition in ("local_ready", "remote_ready", "!yougame::active()",
                          "!Matchmaking::local_peer.enabled"):
            self.assertIn(condition, gate.group(1))
        version = prepare[gate.end():].split("if (remote_count == 1)", 1)[0]
        self.assertIn("g_user->PollOnlineCompatibility(&version_error)", version)
        refusal = version.split("if (version < 0) {", 1)[1].split("}", 1)[0]
        for action in ("cleanup_connection();", "g_forced_error = version_error;",
                       "prepare_online_match_state(q);", "return;"):
            self.assertIn(action, refusal)
        self.assertIn("if (version == 0) remote_ready = 0;", version)
        self.assertIn("g_user->ResetOnlineCompatibility();", function(online, "cleanup_connection"))
        self.assertIn("g_user->ResetOnlineCompatibility();", function(online, "handle_online_inputs"))
        ranked = prepare.split("if (yougame::active() && g_last_search.mode == Matchmaking::RANKED)", 1)[1]
        self.assertLess(ranked.index("if (yougame::ranked_set_over())"),
                        ranked.index("g_user->PollOnlineCompatibility("))
        self.assertIn("cleanup_connection(yougame::Leave::Over);", ranked)
        self.assertIn("if (yougame::ranked_hold()) remote_ready = 0;", ranked)

    def test_initial_search_and_connection_check_the_selected_slippi_login(self):
        net = code(source("port/runtime/hle/slippi_net.cpp"))
        search = function(net, "startMatchmaking")
        # Standalone's gate and the agent short circuit must remain ahead of app routing.
        self.assertRegex(search, r"^\s*if \(!yougame::active\(\) && !local_peer.enabled\) \{")
        self.assertIn("if (!user_ || !user_->CheckOnlineCompatibility(&error)) {", search)
        self.assertIn("if (is_mm_terminated_ || state_ != INITIALIZING) return;", search)
        self.assertLess(search.index("if (yougame::agent_test::agent_script)"),
                        search.index("slippi_user_.reset();"))
        agent = search.split("if (yougame::agent_test::agent_script)", 1)[1].split("#endif", 1)[0]
        self.assertIn("startYouGameMatchmaking();", agent)
        self.assertIn("return;", agent)
        self.assertIn("search_settings_.mode == UNRANKED && search_settings_.friend_room.empty()", search)
        self.assertIn("User::GrantedSlippiUser(user_->dir())", search)
        # Since blended Unranked (yougame_blend.h) the app's Slippi search runs beside YouGame's: the
        # selected login's version is checked before its ticket, and a refusal stops Slippi's side only.
        blend = function(net, "blendMatchmaking")
        check = blend.split("slippi_user_->PollOnlineCompatibility(&error)", 1)[1]
        refusal = check.split("if (version < 0) {", 1)[1].split("}", 1)[0]
        self.assertIn("slippi_failed(", refusal)
        self.assertNotIn("startYouGameMatchmaking", refusal)
        self.assertLess(check.index("if (version < 0)"), check.index("openSlippiTicket("))
        commit = blend.split("coord.matched(Side::Slippi", 1)[1]
        self.assertLess(commit.index("yougame::prepare_slippi_match"), commit.index("ingest_ticket("))
        ticket = function(net, "openSlippiTicket")
        self.assertLess(ticket.index("yougame::with_native_udp(create)"), ticket.index("enet_host_create"))
        connecting = function(net, "handleConnecting")
        self.assertRegex(connecting, r"^\s*if \(\(!yougame::active\(\) \|\| slippi_user_\) && !local_peer.enabled\) \{")
        self.assertIn("User* user = slippi_user_ ? slippi_user_.get() : user_;", connecting)
        refusal = connecting.split("if (!user || !user->CheckOnlineCompatibility(&error)) {", 1)[1].split("}", 1)[0]
        self.assertIn("fail(", refusal)
        self.assertIn("return;", refusal)
        self.assertIn("if (is_mm_terminated_ || state_ != OPPONENT_CONNECTING) return;", connecting)

    def test_slippi_transport_survives_identity_refresh_and_restores_yougame(self):
        link = code(source("port/runtime/hle/yougame_link.cpp"))
        prepare = function(link, "prepare_slippi_match")
        self.assertLess(prepare.index("close_room(Leave::Quit)"), prepare.index("retired.wait_for"))
        self.assertLess(prepare.index("retired.wait_for"), prepare.index("select_match_transport(true)"))
        self.assertIn("if (!g_search_generation.current(epoch) || cancelled()) return false;", prepare)
        self.assertIn("yg_enet_enable(0);", prepare)
        select = function(link, "select_match_transport")
        self.assertIn("yg_enet_select(!slippi && g_direct);", select)
        self.assertIn("yg_enet_select(!slippi && g_broker);", select)
        self.assertEqual(function(link, "fetch_identity").count("select_match_transport(g_slippi_match)"), 2)
        self.assertIn("select_match_transport(false);", function(link, "find_match"))
        self.assertIn("select_match_transport(false);", function(link, "end_session"))
        self.assertIn("if (g_slippi_match) return g_rank.header();", function(link, "ranked_header"))

    def test_both_relay_providers_precede_native_xinput(self):
        window = code(source("port/runtime/host/window.cpp"))
        state = function(window, "xinput_state")
        provider = state.index("if (controller_relay::active())")
        bridge = state.index("if (yougame::relay_pads::active())")
        native = state.index("xinput_ask(")
        # The provider first (0.9.66 had the raw relay first, which hid the provider: every relayed
        # PlayStation and Switch pad also read as an Xbox pad).
        self.assertLess(provider, bridge)
        self.assertLess(bridge, native)
        self.assertIn("return yougame::relay_pads::read(idx, state);", state[bridge:native])
        sample = state[provider:bridge]
        self.assertIn("*state={};", sample)
        self.assertIn("if (!controller_relay::sample(CaptureDevice::XInputPad,idx,s)) return false;", sample)
        for field in ("wButtons", "bLeftTrigger", "bRightTrigger", "sThumbLX", "sThumbLY", "sThumbRX", "sThumbRY"):
            self.assertIn(field, sample)
        self.assertIn("return true;", sample)
        probe = function(window, "xinput_probe_start")
        # Check both before creating the worker and in its loop, before each native probe.
        before_worker, worker = probe.split("std::thread", 1)
        before_probe = worker.split("xinput_ask(", 1)[0]
        for part in (before_worker, before_probe):
            for provider in ("yougame::relay_pads", "controller_relay"):
                self.assertIn("if (" + provider + "::active()) return;", part)
        self.assertIn("controller_relay::rumble(src.kind,src.index,on)", function(window, "input_rumble"))
        self.assertIn("controller_relay::rumble(src.kind,src.index,on)", function(window, "input_rumble_local"))

    def test_desync_trace_retention_keeps_agent_path_cleanup(self):
        trace = source("port/runtime/host/net_trace_file.cpp")
        record = trace.split("case Command::Row:", 1)[1].split("case Command::End:", 1)[0]
        self.assertRegex(record, r"if \(command.record.flags & \([^)]*\bkDesync\b[^)]*\)\) marked_ = true;")
        close = function(code(trace), "close_file")
        discard = close.split("if (rows_ == 0 || !marked_) {", 1)[1].split("} else if", 1)[0]
        self.assertRegex(discard, r"#ifdef YOUGAME_AGENT_TEST_HOOK\s+yougame::agent_paths::remove_output\(path_\);")
        self.assertRegex(discard, r"#else\s+std::remove\(path_\.c_str\(\)\);")
        self.assertIn("yougame::agent_paths::rename_output(path_, final_, ec);", close)

    def test_profiles_keep_all_actions_without_platform_headers(self):
        header = source("port/runtime/host/controller_profiles.h")
        profiles = source("port/runtime/host/controller_profiles.cpp")
        self.assertIn('#include "input_binding_types.h"', header)
        self.assertIn("kProfileActions = static_cast<int>(BindAction::Count)", header)
        self.assertIn("std::size(kActions) == static_cast<size_t>(BindAction::Count)", profiles)
        for text in (header, profiles):
            for include in ('"host.h"', '"input_bindings.h"', '<windows.h>'):
                self.assertNotIn("#include " + include, text)
        actions = profiles.split("constexpr const char* kActions[] = {", 1)[1].split("};", 1)[0]
        for action in ("ModX", "ModY", "LightShield", "MidShield", "StickMod"):
            self.assertEqual(actions.count('"' + action + '"'), 1)

    def test_cmake_retains_upstream_targets_and_shared_profile_tests(self):
        port = re.sub(r"#[^\n]*", "", source("port/CMakeLists.txt"))
        root = source("CMakeLists.txt")
        shared = source("port/tests/controller_profiles.cmake")
        for target in ("temporal_history", "controller_relay"):
            self.assertEqual(port.count("add_executable(port_" + target + "_test "), 1)
            self.assertEqual(port.count("add_test(NAME port_" + target + " "), 1)
        for target in ("controller_profiles", "dolphin_profile"):
            self.assertNotIn("add_executable(port_" + target + "_test", port)
            self.assertEqual(shared.count("add_executable(port_" + target + "_test"), 1)
        self.assertIn("include(port/tests/controller_profiles.cmake)", root)
        self.assertIn('option(MELEE_REQUIRE_STREAMLINE ', port)
        self.assertIn("if(MELEE_REQUIRE_STREAMLINE AND NOT", port)
        self.assertEqual(port.count("IMGUI_IMPL_WIN32_DISABLE_GAMEPAD"), 2)
        for target in ("stage_read_pin", "yougame_agent_fight"):
            self.assertIn("add_executable(port_" + target + "_test", port)
        # Mode policy is shared before the native-Linux early return.
        self.assertIn("add_executable(port_yougame_mode_policy_test", root)
        self.assertLess(root.index("add_test(NAME port_yougame_mode_policy "), root.index("  return()"))
        self.assertIn('option(YOUGAME_AGENT_TEST_HOOK ', port)
        self.assertIn("add_test(NAME port_yougame_0965_merge ", root)


if __name__ == "__main__":
    unittest.main()
