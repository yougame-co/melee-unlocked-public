"""Native menu ownership and raw game-poll wiring for YouGame Friends."""
from pathlib import Path
import unittest
import re

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

    def test_scene_owns_friends_entry_and_pickers_start_only_on_friends_row(self):
        native = source("port/runtime/hle/native_practice.cpp")
        tick = native[native.index("void tick() {"):]
        clear = tick.index("if (major != 1) yougame::friends::events.online_menu(false);")
        self.assertLess(clear, tick.index("if (yougame::active() && !slippi::playback::enabled())"))
        self.assertLess(clear, tick.index("events.take_host()"))
        # Build 12: Direct no longer owns a YouGame picker.
        self.assertIn("const bool picker_scene = major == 1;", tick)
        self.assertIn("if (g_friend_picker.valid() && (!picker_scene || !safe || !yougame::friends::events.picker_context()))", tick)
        self.assertIn("if (picker_scene && host_friend && !g_friend_picker.valid() && room.empty() && !yougame::console::entry.active())", tick)

    def test_source_friends_hands_room_to_console_and_reuses_waiting_panel(self):
        native = source("port/runtime/hle/native_practice.cpp")
        handoff = native[native.index("if (!room.empty()) {", native.index("void tick()")):]
        console = handoff.index("yougame::console::friends_entry(room);")
        self.assertIn("if (g_bridge)", handoff[:console])
        self.assertLess(console, handoff.index("} else {"))
        self.assertLess(console, handoff.index("slippi::online::native_start_match"))
        panel = source("port/runtime/gx/pc_settings.cpp")
        self.assertIn('ImGui::Begin("##yougame_friend_waiting"', panel)
        self.assertIn("yougame::console::entry.status()", panel)
        self.assertNotIn('##console_waiting', panel)

    def test_menu_publishes_selection_after_cursor_movement(self):
        menu = source("sourceport/game/shim/mu_slippi_menu.c")
        think = menu[menu.index("void mu_slippi_online_menu_think"):menu.index("/* ---- the online major")]
        send = think.index("CMD_YOUGAME_FRIENDS_MENU, &friends_menu, 1")
        self.assertGreater(send, think.index("mn_804A04F0.hovered_selection++;"))
        self.assertGreater(send, think.index("mn_804A04F0.hovered_selection--;"))
        self.assertIn("mn_804D6BC8.cooldown = 5;", think[:think.index("} else if ((buttons & MenuInput_Confirm) && ranked_supported")])
        self.assertEqual(think.count("friends_visible = 0;"), 3)  # Confirm elsewhere and Back

    def test_css_exit_is_logged_before_menu_cleanup(self):
        menu = source("sourceport/game/shim/mu_slippi_menu.c")
        decide = menu[menu.index('static void css_decide'):menu.index('/* SSSScenePrep')]
        self.assertLess(decide.index('slippi CSS: exit mode'), decide.index('gm_ChangeGameModeAfterCurrentScene(GM_MENU)'))
        loaded = menu[menu.index('void mu_slippi_main_menu_loaded'):menu.index('/* OnlineModeOptionSelected')]
        self.assertIn('mu_slippi_cleanup_connections();', loaded)

    def test_custom_help_uses_retail_description_box_for_yougame_only(self):
        patch = source("sourceport/patches/melee-native.patch")
        start = patch.index('+        if (menu_kind == MENU_KIND_8 && mu_slippi_menus_enabled() && mu_slippi_yougame_menu()) {')
        help = patch[start:patch.index('+#endif', start)]
        for option, message in (
            ('MU_SLP_OPT_FRIENDS', 'Invite a friend to play.'),
            ('MU_SLP_OPT_SLIPPI', "Direct, Teams and Party on Slippi's servers."),
            ('MU_SLP_MODE_DIRECT', 'Play a Slippi friend by connect code.'),
            ('MU_SLP_MODE_TEAMS', "Slippi's 2v2 teams queue."),
            ('MU_SLP_MODE_PARTY', "Slippi's party mode."),
        ):
            self.assertIn('case ' + option + ':\n+                mu_slippi_menu_description(text, "' + message + '");', help)
        self.assertNotIn('case MU_SLP_MODE_RANKED:', help)
        self.assertNotIn('case MU_SLP_MODE_UNRANKED:', help)
        # The hook runs AFTER the retail constructor, font parameters and SIS
        # catalog lookup. Our helper preserves those fields and copies its prefix.
        before = patch[patch.rindex('static void mn_80229A7C', 0, start):start]
        for line in ('text->font_size.x = 0.0521f;', 'text->font_size.y = 0.0521f;',
                     'HSD_SisLib_803A6368(text, sis_idx[selection]);'):
            self.assertIn(line, before)
        label = source("sourceport/game/shim/mu_menu_label.c")
        helper = label[label.index('void mu_slippi_menu_description'):label.index('void mu_slippi_signin_notice')]
        for field in ('pos_x', 'pos_y', 'pos_z', 'font_size', 'box_size', 'x34', 'default_kerning'):
            self.assertNotIn('text->' + field, helper)
        self.assertIn('mu_menu_description_prefix(text->sis_buffer)', helper)
        self.assertIn('memcpy(stream, text->sis_buffer, prefix)', helper)

    def test_display_rows_are_unlocked_and_cursor_wraps_through_them(self):
        menu = source("sourceport/game/shim/mu_slippi_menu.c")
        self.assertIn('if (ranked_supported) return sel >= 0 && sel < mu_slippi_menu_count();', menu)
        think = menu[menu.index('void mu_slippi_online_menu_think'):menu.index('/* ---- the online major')]
        self.assertEqual(think.count('mu_slippi_menu_count() - 1'), 2)
        self.assertEqual(think.count('while (!mn_80229938(MENU_KIND_8, mn_804A04F0.hovered_selection))'), 2)
        self.assertIn('if ((buttons & MenuInput_Confirm) && friends_visible &&', think)
        self.assertIn('mu_slippi_menu_option(mn_804A04F0.hovered_selection) == MU_SLP_OPT_FRIENDS)', think)

    def test_native_rows_map_to_existing_mode_ids_and_use_yougame_flag(self):
        api = source("sourceport/game/include/mu_native.h")
        self.assertIn("MU_SLP_OPT_LOGIN = 5, MU_SLP_OPT_LOGOUT = 6, MU_SLP_OPT_UPDATE = 7, MU_SLP_OPT_FRIENDS = 8", api)
        patch = source("sourceport/patches/melee-native.patch")
        self.assertIn("selection_count = mu_slippi_menu_count();", patch)
        menu = source("sourceport/game/shim/mu_slippi_menu.c")
        self.assertIn("friends_offered = (r[0] & 2) != 0;", menu)
        self.assertIn("mu_slippi_menu_option", menu)
        self.assertIn("CMD_YOUGAME_FRIENDS_MENU, &friends_menu, 1", menu)
        host = source("port/runtime/hle/slippi_online.cpp")
        self.assertIn("friends_offered(yougame::active()) ? 2 : 0", host)
        self.assertIn("#define YOUGAME_FRIENDS_ROW 1", source("port/runtime/hle/yougame_mode_policy.h"))
        self.assertIn("events.online_menu(yougame::active() && (payload[0] & 3) != 0, (payload[0] & 3) == 2)", host)

    def test_custom_texture_swap_uses_native_and_baked_art(self):
        label = source("sourceport/game/shim/mu_menu_label.c")
        row = label[label.index('void mu_slippi_friends_label'):label.index('void mu_slippi_menu_labels_init')]
        self.assertIn('!mu_slippi_yougame_menu()', row)
        self.assertIn('menu_kind != MENU_KIND_8', row)
        self.assertIn('if (label == NULL) return;', row)
        self.assertIn('native[selection] = original;', row)
        self.assertIn('replacement = native[2 + selection];', row)
        self.assertIn('kind == 1 ? mu_baked_friends : mu_baked_slippi', row)
        self.assertIn('replacement->width == original->width', row)
        self.assertIn('replacement->height == original->height', row)
        self.assertIn('replacement->format == original->format', row)
        self.assertNotIn('texture->imagetbl =', row)
        self.assertNotIn('original->image_ptr =', row)
        self.assertNotIn('HSD_SisLib_FontAtlas', label)
        self.assertNotIn('mu_menu_label_render', label)
        init = label[label.index('void mu_slippi_menu_labels_init'):label.index("/* Keep the game's text object")]
        self.assertIn('slot < 8', init)
        self.assertIn('140 + 2 * slot', init)
        self.assertIn('mu_slippi_friends_label(parts[1], menu_kind, slot, 1);', init)
        self.assertLess(init.index('mu_slippi_friends_label'), init.index('HSD_JObjRemoveAll'))
        self.assertIn('mu_baked_slippi_title', label)
        self.assertNotIn('mu_menu_title_render', label)

    def test_label_adapter_never_requests_or_animates_texture_art(self):
        label = source("sourceport/game/shim/mu_menu_label.c")
        self.assertNotRegex(label, r'\bHSD_TObj(?:ReqAnim|Anim)\s*\(')
        self.assertNotIn('mu_slippi_menu_art', label)
        self.assertNotIn('mu_menu_glyphs', source("sourceport/game/shim/mu_menu_label.h"))

    def test_texture_is_reapplied_after_creation_hover_and_unhover_animation(self):
        patch = source("sourceport/patches/melee-native.patch")
        for obj, menu, row, indent in (
            ('jobj', 'flow->cur_menu', 'selection', 4),
            ('r29', 'flow->cur_menu', 'selection', 4),
            ('jobj.value', 'menu_kind', 'i', 12),
        ):
            call = f'mu_slippi_friends_label({obj}, {menu}, {row}, 0);'
            self.assertEqual(patch.count(call), 1)
            prefix = (f'HSD_JObjAnim({obj});\n+#ifdef MU_NATIVE\n+' +
                      ('            if (i == 0) mu_slippi_menu_labels_init(&MenMainCursor_Top, menu_kind);\n+' if row == 'i' else '') + ' ' * indent)
            self.assertIn(prefix +
                          f'if (mu_slippi_menu_label_kind({row}))\n+' + ' ' * (indent + 4) + call + '\n+#endif', patch)
        # The preview and existing Update frame retain their original ownership.
        self.assertIn('+        anim[i] = original[source];', patch)
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
