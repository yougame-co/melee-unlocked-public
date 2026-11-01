"""Phob menu shares gameplay's app mirror, with a hook-only boot request."""
from pathlib import Path
import unittest
import re
from relay_pads_wiring_test import code, block
def function(text, name):
    # Locate braces in masked C++, but keep string literals in the returned body.
    masked = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/',
                    lambda m: ' ' * len(m.group()), text, flags=re.S)
    match = re.search(r'\b' + name + r'\([^;]*?\)\s*\{', masked)
    if not match: raise AssertionError('Missing function: ' + name)
    body = block(masked, match.end())
    return text[match.end():match.end() + len(body)]
ROOT = Path(__file__).resolve().parents[2]
def source(p): return (ROOT / p).read_text(encoding='utf-8-sig')
class PhobMirrorWiringTest(unittest.TestCase):
    def test_menu_reads_scope_and_raw_from_host(self):
        menu = source('port/runtime/gx/phob_menu.cpp')
        self.assertIn('host::gcadapter_scope(port, out.data(), n)', function(menu, 'latest'))
        self.assertIn('host::gcadapter_phob_raw(port, hw)', menu)
        self.assertIn('latest(port, v, 1)', function(menu, 'draw_card'))
        self.assertIn('have ? sub : "No controller in this socket"', menu)
    def test_mirror_raw_uses_gameplay_origin_and_no_usb(self):
        raw = source('port/runtime/host/gc_adapter.cpp')
        # Second definition is Windows/Linux; first is macOS.
        start = raw.index('bool gcadapter_phob_raw(', raw.index('#else'))
        body = function(raw[start:], 'gcadapter_phob_raw')
        self.assertIn('if (yougame::stdio_bridge())', body)
        self.assertIn('attach_mirror()', body)
        self.assertIn('mirror_live(report) && phob::mirror_input(report, port, g_origin[port], in)', body)
        self.assertLess(body.index('mirror_live(report)'), body.index('!g_have_report'))
        for usb in ('libusb_', 'ensure_scanner_started(', 'CreateFile'): self.assertNotIn(usb, body)
        poll = function(raw, 'gcadapter_poll')
        self.assertIn('read_mirror(rep)', raw)
        self.assertIn('read_mirror(rep)', poll)
        self.assertIn('mirror_live(rep)', source('port/runtime/host/gc_adapter_mirror_mac.h'))
        self.assertIn('bool gcadapter_phob_raw(int, phob::HardwareInput&) { return false; }', raw)
    def test_mirror_live_sample_and_direct_history(self):
        raw = source('port/runtime/host/gc_adapter.cpp')
        start = raw.index('int gcadapter_scope(', raw.index('#else'))
        body = function(raw[start:], 'gcadapter_scope')
        self.assertIn('if (yougame::stdio_bridge())', body)
        self.assertIn('if (!gcadapter_phob_raw(port, in)) return 0;', body)
        self.assertIn('phob::live_sample<GCScopeSample>', body)
        self.assertIn('g_scope[port]', body)
    def test_diag_only_live_panel_and_bounded(self):
        card = function(source('port/runtime/gx/phob_menu.cpp'), 'draw_card')
        self.assertIn('limiter.due(diag, g_in.time)', card)
        self.assertIn('std::strcmp(value, "1") == 0', card)
        self.assertIn('ui diag: phob menu socket %d source %s connected %d stick %d,%d', card)
        self.assertIn('yougame::stdio_bridge() ? "mirror" : have ? "direct" : "none"', card)
        self.assertLess(card.index('case INPUTVIEW:'), card.index('limiter.due'))
        self.assertIn('next = now + 1.0;', source('port/runtime/host/phob_adapter_input.h'))
    def test_hook_key_and_consumer_compile_guards(self):
        for path, text in [('port/runtime/hle/yougame_agent_test.h', '    if (key == "openMenu")'),
                           ('port/runtime/gx/pc_settings.cpp', '  if (const int socket = yougame::agent_test::phob_menu_socket.exchange(0))')]:
            raw = source(path); prefix = raw[:raw.index(text)]
            self.assertGreater(prefix.rfind('#ifdef YOUGAME_AGENT_TEST_HOOK'), prefix.rfind('#endif'))
        ui = source('port/runtime/gx/pc_settings.cpp')
        self.assertIn('state.active_tab = 3;', ui)
        self.assertIn('phob_native_request(socket - 1);', ui)
        hook = source('port/runtime/hle/yougame_agent_test.h')
        self.assertIn('phob_menu_socket.store(out.phob_socket);', hook)
        self.assertIn('phob_menu_socket.store(0);', hook)
        self.assertIn('openMenu|phob:', source('port/tests/agent_test_hook_absent.sh'))
    def test_targets_registered_before_linux_return(self):
        root = source('CMakeLists.txt')
        self.assertLess(root.index('add_executable(port_phob_mirror_hook_test'), root.index('  return()'))
        for target in ('port_phob_mirror_test', 'port_phob_mirror_hook_test'):
            self.assertIn("'" + target + "'", source('tools/build_linux.py'))
if __name__ == '__main__': unittest.main()
