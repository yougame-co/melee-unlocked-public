"""Every WinHTTP WebSocket call the room socket makes has a Linux stand-in, the close status included."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class LinuxWinHttpCloseStatusTest(unittest.TestCase):
    def test_room_socket_calls_exist_on_linux(self):
        link = (ROOT / "port/runtime/hle/yougame_link.cpp").read_text(encoding="utf-8")
        header = (ROOT / "port/linux/compat/winhttp.h").read_text(encoding="utf-8")
        for name in sorted(set(re.findall(r"\b(WinHttpWebSocket\w+)\(", link))):
            self.assertRegex(header, rf"\b{name}\(", f"{name} has no Linux declaration")

    def test_close_status_reads_the_peers_close_frame(self):
        src = (ROOT / "port/linux/compat/winhttp_ws.cpp").read_text(encoding="utf-8")
        self.assertIn("ws->close_frame = control; ws->closed_by_peer = true;", src)
        body = src[src.index("DWORD WinHttpWebSocketQueryCloseStatus("):]
        body = body[:body.index("\n}\n")]
        self.assertIn("if (!ws->closed_by_peer) return 4317;", body)
        self.assertIn("USHORT((frame[0] << 8) | frame[1]) : USHORT(1005)", body)


if __name__ == "__main__":
    unittest.main()
