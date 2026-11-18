"""122.19: the online character select's prompts name a keyboard player's own keys, and a closed Doubles
home lets B out.

- "Hold Z to disconnect" and the other Z (and B) prompts: Melee's Z is E on the keyboard by default, so the
  prompt asks the host for the key bound to that button on a keyboard port (private command 0xDF), and
  keeps the GameCube name for a controller. What the keys do is unchanged: the keyboard's Z key is still A.
- A Doubles home whose room closed while it searched kept MU_LOBBY_M_SEARCHING (Rooms never sends the end
  of a search once the room is gone). The select takes the whole pad while it searches (mu_lobby_pads), so
  "Lobby closed. Hold B to exit" could not be left. The behaviour is unit tested in
  port/tests/yougame_lobby_screen_test.cpp; here, the wiring.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


def body(source, name):
    at = source.index(name)
    at = source.index('{', at)
    depth, end = 1, at + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[at:end]


class KeyboardPromptTest(unittest.TestCase):
    def setUp(self):
        self.css = read("sourceport/game/shim/mu_slippi_css.c")
        self.host = read("port/app/source_host.cpp")
        self.window = read("port/runtime/host/window.cpp")

    def test_shim_asks_the_host(self):
        self.assertIn("#define CMD_BUTTON_LABEL 0xDF", self.css)
        fn = body(self.css, "int mu_host_button_label(int port, unsigned button_mask, char* out, int n)")
        self.assertIn("mu_online_abi_command(CMD_BUTTON_LABEL", fn)
        self.assertIn("reply[4096]", fn)   # the host refuses a reply buffer under its minimum capacity
        self.assertIn("got >= (unsigned int) n", fn)
        self.assertIn("int mu_host_button_label(int port, unsigned button_mask, char* out, int n);",
                      read("sourceport/game/include/mu_native.h"))

    def test_prompts_use_the_bound_keys(self):
        think = body(self.css, "static void text_think(HSD_GObj* gobj)")
        self.assertIn('char z_key[16] = "Z", b_key[16] = "B"', think)
        self.assertIn("mu_host_button_label(*ctrl_port, HSD_PAD_Z, z_key, sizeof z_key);", think)
        self.assertIn("mu_host_button_label(*ctrl_port, HSD_PAD_B, b_key, sizeof b_key);", think)
        for line in ('"Hold %s to disconnect", z_key', '"Press %s, then hold %s to update", z_key, b_key',
                     '"Press %s to clear error", z_key', '"Press %s to cancel", z_key',
                     '"%s: Practice vs CPU", z_key', '"Match closed. Hold %s to exit", b_key'):
            self.assertIn(line, think)
        for old in ('"Hold Z to %s"', '"Press Z, then hold B to update"', '"Press Z to %s"',
                    '"Z: Practice vs CPU"', '"Match closed. Hold B to exit"'):
            self.assertNotIn(old, think)

    def test_inputs_unchanged(self):
        # Only the words change: Z is still the pad's Z bit, read from the CSS's own port.
        inputs = body(self.css, "int mu_slippi_css_inputs(unsigned int trigger)")
        self.assertIn("gm_GetButtonsPressed((u8) *ctrl_port) & HSD_PAD_Z", inputs)
        self.assertNotIn("mu_host_button_label", inputs)

    def test_host_answers_keyboard_ports_only(self):
        self.assertIn("constexpr uint8_t CMD_BUTTON_LABEL = 0xDF;", self.host)
        handler = self.host[self.host.index("if (c == CMD_BUTTON_LABEL && n == 3)"):]
        handler = handler[:handler.index("return true;")]
        self.assertIn("host::input_keyboard_button_label(p[0], (uint16_t)(p[1] << 8 | p[2])", handler)
        # Console sessions answer nothing: every machine draws the same select there.
        self.assertRegex(self.host, r"command == CMD_BUTTON_LABEL\)\) \{   // the same on every console machine")
        fn = body(self.window, "bool input_keyboard_button_label(int port, uint16_t pad_button, char* out, size_t n)")
        self.assertIn("g_keyboard_port_mask.load", fn)
        self.assertIn("kActionPadBit[i] == pad_button", fn)
        self.assertIn("g_key_bindings.vk[i]", fn)   # the live binding, never a fixed key
        self.assertNotIn("'E'", fn)
        # The mask is the same keyboard_ports both reads publish, set before the UI lock.
        snap = self.window[self.window.index("struct UiSnapshot {"):self.window.index("void input_poll(PadState out[4]) {")]
        self.assertIn("if (keyboard_ports[i]) keyboard_mask |= (uint8_t)(1u << i);", snap)
        self.assertIn("g_keyboard_port_mask.store(keyboard_mask", snap)
        self.assertIn("bool input_keyboard_button_label(int port, uint16_t pad_button, char* out, size_t n);",
                      read("port/runtime/host/host.h"))


class ClosedHomeBackTest(unittest.TestCase):
    def test_closed_room_is_never_searching(self):
        screen = read("port/runtime/hle/yougame_lobby_screen.h")
        fn = body(screen, "inline MuLobbyView screen_view(const Control::View& v, const ScreenFlags& f)")
        self.assertIn("doubles && v.searching && !f.closed ? MU_LOBBY_M_SEARCHING : 0", fn)
        self.assertIn("f.armed && f.closed ? MU_LOBBY_HINT_CLOSED", fn)

    def test_searching_takes_the_pad(self):
        # Why it mattered: the searching branch zeroes the player's pad, B included.
        pads = body(read("sourceport/game/shim/mu_slippi_menu.c"), "void mu_lobby_pads(PADStatus* status)")
        search = pads[pads.index("if (lobby.view_ok && (lobby.view.mode & MU_LOBBY_M_SEARCHING))"):]
        search = search[:search.index("} else if")]
        self.assertIn("memset(&mine, 0, sizeof mine);", search)

    def test_unit_test_pins_it(self):
        test = read("port/tests/yougame_lobby_screen_test.cpp")
        self.assertIn("nor keep a closed Doubles home searching", test)


if __name__ == "__main__":
    unittest.main()
