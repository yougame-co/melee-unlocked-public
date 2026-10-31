"""Native menu ownership and raw game-poll wiring for YouGame Friends."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]

def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")

class FriendsMenuWiringTest(unittest.TestCase):
    def test_game_poll_uses_unclaimed_buttons_and_honors_ui_capture(self):
        window = source("port/runtime/host/window.cpp")
        poll = window.index("yougame::friends::events.poll_pad(i, pads[i].err == 0 && invite_focused, menu_buttons)")
        overlay = window.index("g_overlay_pad.poll(yougame::stdio_bridge(), i,")
        strip = window.index("if (!(menu_buttons & 0x1000)) pads[i].button")
        self.assertLess(overlay, poll)
        self.assertLess(poll, strip)
        self.assertIn("if (yougame::active() && !opened_here && !g_ui_capture.load())", window[overlay:poll])
        self.assertIn("const uint16_t raw_buttons = pads[i].button;", window[:poll])
        ui = source("port/runtime/gx/pc_settings.cpp")
        self.assertNotIn("window_ui_gamecube_pad(friend_pad)", ui)
        self.assertNotIn('ImGui::Selectable("Friends"', ui)
        self.assertNotIn('##yougame_online_friends', ui)

    def test_scene_owns_friends_entry_and_pickers_start_only_on_main_menu_or_code_box(self):
        native = source("port/runtime/hle/native_practice.cpp")
        tick = native[native.index("void tick() {"):]
        clear = tick.index("if (major != 1) yougame::friends::events.online_menu(false);")
        self.assertLess(clear, tick.index("if (yougame::active() && !slippi::playback::enabled())"))
        self.assertLess(clear, tick.index("events.take_host()"))
        # 0b0028 regression (quality, pc2 10-30): the Direct code box lives in the CSS scene, so a picker must also start there.
        self.assertIn("const bool picker_scene = major == 1 || yougame::friends::events.code_screen();", tick)
        self.assertIn("if (g_friend_picker.valid() && (!picker_scene || !safe || !yougame::friends::events.picker_context()))", tick)
        self.assertIn("if (picker_scene && host_friend && !g_friend_picker.valid() && room.empty())", tick)

    def test_menu_publishes_selection_after_cursor_movement(self):
        menu = source("sourceport/game/shim/mu_slippi_menu.c")
        think = menu[menu.index("void mu_slippi_online_menu_think"):menu.index("/* ---- the online major")]
        send = think.index("CMD_YOUGAME_FRIENDS_MENU, &friends_menu, 1")
        self.assertGreater(send, think.index("mn_804A04F0.hovered_selection++;"))
        self.assertGreater(send, think.index("mn_804A04F0.hovered_selection--;"))
        self.assertNotIn("return;", think)
        self.assertIn("mn_804D6BC8.cooldown = 5;", think[:think.index("} else if (buttons & MenuInput_Confirm)")])
        self.assertEqual(think.count("friends_visible = 0;"), 2)  # Confirm elsewhere and Back

    def test_friends_help_uses_game_description_box_only_when_offered(self):
        patch = source("sourceport/patches/melee-native.patch")
        start = patch.index('+        if (menu_kind == MENU_KIND_8 && selection == MU_SLP_OPT_FRIENDS &&')
        help = patch[start:patch.index('+#endif', start)]
        self.assertIn('mu_slippi_menus_enabled() && mu_slippi_friends_available()', help)
        self.assertIn('HSD_SisLib_803A6754(0, mn_804D6BB4)', help)
        self.assertIn('text->pos_x = -9.5f;', help)
        self.assertIn('text->pos_y = 9.1f;', help)
        self.assertIn('text->box_size_x = 364.68332f;', help)
        self.assertIn('text->box_size_y = 38.38772f;', help)
        self.assertIn('"Invite a friend to play."', help)
        self.assertIn('data->description = text;', help)
        self.assertIn('return;', help)
        self.assertIn('+#ifdef MU_NATIVE', patch[start - 30:start])

    def test_slot_eight_stays_unlocked_and_cursor_wraps_through_it(self):
        menu = source("sourceport/game/shim/mu_slippi_menu.c")
        self.assertIn('if (sel == MU_SLP_OPT_FRIENDS) return slp.app_state == MU_SLP_APP_LOGGED_IN && friends_offered;', menu)
        think = menu[menu.index('void mu_slippi_online_menu_think'):menu.index('/* ---- the online major')]
        self.assertEqual(think.count('MU_SLP_OPT_COUNT - 1'), 2)
        self.assertEqual(think.count('while (!mn_80229938(MENU_KIND_8, mn_804A04F0.hovered_selection))'), 2)
        self.assertIn('if ((buttons & MenuInput_Confirm) && friends_visible &&', think)
        self.assertIn('mn_804A04F0.hovered_selection == MU_SLP_OPT_FRIENDS)', think)

    def test_native_row_appends_without_changing_existing_ids_and_uses_yougame_flag(self):
        api = source("sourceport/game/include/mu_native.h")
        self.assertIn("MU_SLP_OPT_LOGIN = 5, MU_SLP_OPT_LOGOUT = 6, MU_SLP_OPT_UPDATE = 7, MU_SLP_OPT_FRIENDS = 8", api)
        patch = source("sourceport/patches/melee-native.patch")
        self.assertIn("selection_count = mu_slippi_friends_available() ? 9 : 8;", patch)
        menu = source("sourceport/game/shim/mu_slippi_menu.c")
        self.assertIn("friends_offered = (r[0] & 2) != 0;", menu)
        self.assertIn("sel == MU_SLP_OPT_FRIENDS", menu)
        self.assertIn("CMD_YOUGAME_FRIENDS_MENU, &friends_menu, 1", menu)
        host = source("port/runtime/hle/slippi_online.cpp")
        self.assertIn("friends_offered(yougame::active()) ? 2 : 0", host)
        self.assertIn("events.online_menu(yougame::active() && payload[0] != 0, payload[0] == 2)", host)

    def test_friends_texture_is_native_and_only_targets_yougame_slot_eight(self):
        label = source("sourceport/game/shim/mu_menu_label.c")
        guard = label[label.index('if (!mu_slippi_menus_enabled()'):label.index('if (!ready)')]
        self.assertIn('!mu_slippi_friends_available()', guard)
        self.assertIn('menu_kind != MENU_KIND_8', guard)
        self.assertIn('selection != MU_SLP_OPT_FRIENDS', guard)
        self.assertIn('label == NULL) return;', guard)
        self.assertIn('texture->imagedesc = &image;', label)
        self.assertNotIn('texture->imagetbl =', label)
        self.assertNotIn('original->image_ptr =', label)
        self.assertIn('HSD_SisLib_FontAtlas', label)
        renderer = source("sourceport/game/shim/mu_menu_label.h")
        self.assertIn('static const char text[] = "Friends";', renderer)
        build = source("sourceport/game/CMakeLists.txt")
        self.assertIn('file(GLOB_RECURSE SHIM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shim/*.c")', build)

    def test_texture_is_reapplied_after_creation_hover_and_unhover_animation(self):
        patch = source("sourceport/patches/melee-native.patch")
        for animation, call in (
            ('HSD_JObjAnim(jobj);', 'mu_slippi_friends_label(jobj, flow->cur_menu, selection);'),
            ('HSD_JObjAnim(r29);', 'mu_slippi_friends_label(r29, flow->cur_menu, selection);'),
            ('HSD_JObjAnim(jobj.value);', 'mu_slippi_friends_label(jobj.value, menu_kind, i);'),
        ):
            self.assertEqual(patch.count('+' + ' ' * (12 if 'jobj.value' in call else 4) + call), 1)
            self.assertIn(animation + '\n+#ifdef MU_NATIVE\n+' +
                          ' ' * (12 if 'jobj.value' in call else 4) + call + '\n+#endif', patch)
        # The preview and existing Update frame retain their original ownership.
        self.assertIn('+    anim[8] = anim[2];', patch)
        self.assertIn('+    mn_803EB6B0[MENU_KIND_8].start_frame = 140;', patch)

    def test_mirror_observes_the_match_service_loop(self):
        net = source("port/runtime/hle/slippi_net.cpp")
        match_loop = net.index("  while (do_loop_.load())")
        mirror = net.index("yg_enet_mirror_service(")
        self.assertGreater(mirror, match_loop)
        self.assertLess(mirror, net.index("enet_host_service(client_, &ev, service_wait)", match_loop))
        self.assertLess(mirror, net.index("enet_host_service(client_, &ev, held.empty() ? 250 : 1)", match_loop))

    def test_version_refusal_and_notice_are_handled_before_generic_error(self):
        link = source("port/runtime/hle/yougame_link.cpp")
        version = link.index('if (friends_mode && friends::version_reason(m)')
        error = link.index('if (t == "error") return fail(m.value("error", m.value("message", "The room refused this player"))')
        self.assertLess(version, error)
        self.assertIn("friends::events.take_version()", link)
        self.assertIn("friends::version_request(body) && friends::version_reason(j) ? friends::kVersion", link)

    def test_new_search_discards_an_old_version_notice_before_search_work(self):
        link = source("port/runtime/hle/yougame_link.cpp")
        constructor = link[link.index("explicit FriendSearch(bool active)"):link.index("~FriendSearch()")]
        self.assertLess(constructor.index("friends::events.take_version();"), constructor.index("g_friend_search = active;"))

    def test_joined_status_uses_its_original_timestamp_and_expires(self):
        link = source("port/runtime/hle/yougame_link.cpp")
        setter = link[link.index("void set_status("):link.index("using transport::read_roster;")]
        self.assertIn("g_status_since = now_ms();", setter)
        reader = link[link.index("std::string status_line()"):link.index("std::string pick_friend_room(")]
        self.assertIn("g_status = friends::expire_joined(g_status, g_status_since, now_ms());", reader)
        self.assertNotIn("g_status_since =", reader)

if __name__ == "__main__":
    unittest.main()
