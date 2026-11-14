#!/usr/bin/env python3
"""The local channel is wired where the contract says: never when the app or the launcher started
the game, probed once at start after the log opens, retried when the Online menu opens, and a
connected channel opens the Online rows while a bare start stays "Coming soon".

argv[1]: the source root.
SPDX-License-Identifier: GPL-2.0-or-later
"""
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
failures = []


def read(rel):
    return (root / rel).read_text(encoding="utf-8")


def need(ok, what):
    if not ok:
        failures.append(what)


link = read("port/runtime/hle/yougame_link.cpp")
header = read("port/runtime/hle/yougame_link.h")
main = read("port/app/main.cpp")
online = read("port/runtime/hle/slippi_online.cpp")
channel = read("port/runtime/hle/yougame_channel.cpp")

need((root / "port/runtime/hle/yougame_channel.h").exists(), "yougame_channel.h exists")

# Never when the app (stdio or HTTP) or Melee Unlocked's launcher started the game.
allowed = re.search(r"g_channel_allowed\s*=\s*(!std::getenv[^;]*);", link)
need(allowed is not None, "start() sets g_channel_allowed")
if allowed:
    for name in ("YOUGAME_STDIO", "YOUGAME_BRIDGE", "MELEE_YOUGAME_LAUNCHER"):
        need(f'!std::getenv("{name}")' in allowed.group(1), f"g_channel_allowed checks {name}")
    # Read before take() clears the variables.
    need(allowed.start() < link.index('take("YOUGAME_BRIDGE")'), "allowed is read before take()")

# The bridge sends through the channel only when no app bridge exists.
need(re.search(r"g_stdio\s*\?\s*bridge_stdio\([^)]*\)\s*:\s*g_channel_allowed\s*\?\s*bridge_channel", link) is not None,
     "bridge() routes stdio, then the channel, then HTTP")

# Rows: a connected channel opens them; a bare start keeps "Coming soon".
soon = re.search(r"bool coming_soon\(\)\s*\{(.*?)\n\}", link, re.S)
need(soon is not None and "!g_channel_up" in soon.group(1), "coming_soon() is off while the channel is up")
launcher = re.search(r"bool launcher\(\)\s*\{(.*?)\n\}", link, re.S)
need(launcher is not None and "g_channel_up" in launcher.group(1), "launcher() counts a connected channel")

# One probe at start, after the log is open; a retry when the Online menu opens.
need("void start_channel(bool automated);" in header and "void online_menu_opened();" in header,
     "yougame_link.h declares start_channel and online_menu_opened")
ready = main.find("yougame::relay_pads::logging_ready();")
start = main.find("yougame::start_channel(automated);")
need(ready >= 0 and start > ready, "main.cpp starts the channel after logging_ready()")
need(re.search(r"if \(payload_len == 0\) yougame::online_menu_opened\(\);", online) is not None,
     "the Online menu entry (CMD_YOUGAME_MENU) retries the channel")

# The probe runs on its own thread, and a host's pad-mirror handle is never used.
need(re.search(r"std::thread\(\[[^\]]*\][^)]*\)\s*\{\s*probe_channel\(\)", link) is not None
     or re.search(r"std::thread\([^;]*probe_channel", link, re.S) is not None,
     "the probe runs on a background thread")
need('type == "pads"' in channel and "AppOnly" in channel, "the pads push is skipped on the channel")

# Never a frame on the channel: the menu's retry returns at once, and its identity is never waited for.
menu = re.search(r"void online_menu_opened\(\)\s*\{(.*?)\n\}", link, re.S)
need(menu is not None and "Sleep" not in menu.group(1) and "while" not in menu.group(1), "online_menu_opened() never waits")
need("yougame::channel_connected() ? yougame::identity().ok : yougame::wait_identity(500).ok" in online,
     "the Online menu never waits for the channel's identity")
need("if (!settings_window_only) yougame::start_channel(automated);" in main, "the settings window never probes")
# A game the app started, restarted by itself, still never probes.
need('!std::getenv("MELEE_STARTED_BY_APP")' in link and '_putenv_s("MELEE_STARTED_BY_APP", "1")' in link,
     "the app-started marker is checked and carried to children")
# Up only while current and open (H2); a stale reader never touches the next candidate (W6).
need("channel::may_promote(g_channel, conn)" in link, "the probe promotes only the current, open connection")
closed = re.search(r"void channel_closed\(.*?\n\}", link, re.S)
need(closed is not None and re.search(r"if \(g_channel != conn\) return;.*?std::lock_guard<std::mutex> io\(g_io.mutex\);\s*g_io.eof = true;\s*\}",
                                       closed.group(0), re.S) is not None,
     "channel_closed checks the connection and sets eof in one critical section")
# The menu file: since build 98.2 the rows are never greyed (one copy, the same connected or not).
host = read("port/app/source_host.cpp")
need("const bool soon = false;" in host and "if (at == 0 || !f.serving)" in host, "menu rows never greyed")

# Logs: one line per probe result, never a token.
for line in ("yougame channel: none", "yougame channel: connected ", "yougame channel: refused ",
             "yougame channel: hello answered ", "yougame channel: player arrived by ", "yougame channel: retry hello at ",
             "yougame channel: mpCode answered ", "yougame channel: unparsed line ("):
    need(line in link, f"logs '{line.strip()}'")
need(re.search(r"yougame channel:[^\n]*token", link) is None, "no channel log line names a token")

# A player who arrives while a sign-in ask is in flight is never dropped (two-PC run 11-13): the ask
# that began before them runs once more, and both ask paths share that loop.
def body(name):
    m = re.search(r"\nvoid " + name + r"\(\) \{[^\n]*\n(.*?)\n\}\n", link, re.S)
    return m.group(1) if m else ""
asks, arrived, now, refresh = body("run_identity_asks"), body("player_arrived"), body("fetch_identity_now"), body("refresh_identity")
need("g_identity_again = false;" in asks and "fetch_identity();" in asks
     and "while (g_identity_again.load() && !g_identity_fetching.exchange(true));" in asks
     and -1 < asks.find("g_identity_fetching = false;") < asks.find("while ("), "run_identity_asks re-asks for a player who arrived mid-ask")
need("g_identity_again.exchange(false)" not in link, "only the holder of g_identity_fetching clears g_identity_again")
need(arrived.find("g_identity_again = true;") != -1 and arrived.find("g_identity_again = true;") < arrived.find("refresh_identity();"),
     "player_arrived marks the in-flight ask to run again before refreshing")
need("run_identity_asks();" in now and "fetch_identity();" not in now, "fetch_identity_now uses the shared loop")
need("std::thread(run_identity_asks).detach();" in refresh and "fetch_identity();" not in refresh, "refresh_identity uses the shared loop")

if failures:
    for f in failures:
        print("FAIL:", f)
    sys.exit(1)
print("yougame_channel_wiring: all checks passed")
