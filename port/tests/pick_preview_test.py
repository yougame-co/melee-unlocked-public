"""Live pick previews on the online character screen: the codec, and how host and game use it.

Display only: these never reach match setup (0x82 and 0xE3 still decide the match).
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from squad_wiring_test import GAME, ROOT, function, read, without_squad

ABI = ROOT / 'port/runtime/abi'

CODEC = r'''
#include "mu_pick_preview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static MuPickPreview sample(void)
{
    MuPickPreview p;
    mu_pick_preview_clear(&p);
    p.char_id = 0x14; p.color = 3; p.team_id = 1; p.squad_count = 2;
    p.kinds[0] = 9; p.colors[0] = 1; p.kinds[1] = 2; p.colors[1] = 4; p.locked = 1;
    return p;
}

int main(void)
{
    unsigned char wire[MU_PICK_PREVIEW_WIRE + 2], big[MU_PICK_PREVIEW_WIRE + 2];
    unsigned char reply[MU_PICK_PREVIEW_REPLY + 2];
    MuPickPreview p = sample(), q, bad;
    MuPickPreviewSlot slots[MU_PICK_PREVIEW_REMOTES], back[MU_PICK_PREVIEW_REMOTES];
    MuPickPreviewSender sender;
    size_t n, i;

    CHECK(MU_PICK_PREVIEW_WIRE == 12);
    CHECK(MU_PICK_PREVIEW_REPLY == 1 + 3 * 14);
    /* Round trip, byte for byte. */
    n = mu_pick_preview_encode(&p, wire, sizeof wire);
    CHECK(n == MU_PICK_PREVIEW_WIRE);
    {
        const unsigned char expect[12] = { 1, 0x14, 3, 1, 2, 9, 1, 2, 4, 0xFF, 0, 1 };
        CHECK(memcmp(wire, expect, sizeof expect) == 0);
    }
    memset(&q, 0x55, sizeof q);
    CHECK(mu_pick_preview_decode(wire, n, &q));
    CHECK(memcmp(&p, &q, sizeof p) == 0);
    /* The empty pick (no fighter, no team, no slots) round trips too. */
    mu_pick_preview_clear(&q);
    CHECK(mu_pick_preview_encode(&q, big, sizeof big) == MU_PICK_PREVIEW_WIRE);
    CHECK(mu_pick_preview_decode(big, MU_PICK_PREVIEW_WIRE, &bad));
    CHECK(bad.char_id == 0xFF && bad.team_id == 0xFF && bad.squad_count == 0 && bad.kinds[2] == 0xFF);
    /* Every length but the exact one is refused, including off by one either way. */
    memcpy(big, wire, MU_PICK_PREVIEW_WIRE);
    big[MU_PICK_PREVIEW_WIRE] = 0;
    for (i = 0; i <= MU_PICK_PREVIEW_WIRE + 1; ++i) {
        memset(&q, 0x55, sizeof q);
        CHECK(mu_pick_preview_decode(big, i, &q) == (i == MU_PICK_PREVIEW_WIRE));
        if (i != MU_PICK_PREVIEW_WIRE) CHECK(q.char_id == 0x55);   /* untouched on refusal */
    }
    CHECK(mu_pick_preview_encode(&p, wire, MU_PICK_PREVIEW_WIRE - 1) == 0);
    CHECK(!mu_pick_preview_decode(NULL, MU_PICK_PREVIEW_WIRE, &q));
    CHECK(!mu_pick_preview_decode(wire, MU_PICK_PREVIEW_WIRE, NULL));
    /* Wrong versions. */
    for (i = 0; i < 256; ++i) {
        memcpy(big, wire, MU_PICK_PREVIEW_WIRE);
        big[0] = (unsigned char) i;
        CHECK(mu_pick_preview_decode(big, MU_PICK_PREVIEW_WIRE, &q) == (i == MU_PICK_PREVIEW_VERSION));
    }
    /* Non-canonical content: too many slots, a filled empty slot, an empty filled slot, flags. */
    {
        const struct { size_t at; unsigned char value; } cases[] = {
            { 4, 4 }, { 4, 0xFF }, { 9, 5 }, { 10, 1 }, { 5, 0xFF }, { 11, 2 }, { 3, 3 }, { 3, 0x80 },
        };
        for (i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
            memcpy(big, wire, MU_PICK_PREVIEW_WIRE);
            big[cases[i].at] = cases[i].value;
            CHECK(!mu_pick_preview_decode(big, MU_PICK_PREVIEW_WIRE, &q));
        }
    }
    bad = p; bad.squad_count = 4;
    CHECK(mu_pick_preview_encode(&bad, wire, sizeof wire) == 0);
    bad = p; bad.locked = 2;
    CHECK(mu_pick_preview_encode(&bad, wire, sizeof wire) == 0);
    n = mu_pick_preview_encode(&p, wire, sizeof wire);

    /* Rate limit: the first send at once, a repeat never, a change only after the interval
     * (and then without another change), force at once. */
    memset(&sender, 0, sizeof sender);
    CHECK(mu_pick_preview_due(&sender, wire, 100, 0));
    CHECK(!mu_pick_preview_due(&sender, wire, 101, 0));
    CHECK(!mu_pick_preview_due(&sender, wire, 200, 0));
    memcpy(big, wire, MU_PICK_PREVIEW_WIRE); big[2] = 4;   /* another colour */
    memset(&sender, 0, sizeof sender);
    CHECK(mu_pick_preview_due(&sender, wire, 100, 0));
    for (i = 101; i < 100 + MU_PICK_PREVIEW_INTERVAL; ++i) CHECK(!mu_pick_preview_due(&sender, big, (unsigned) i, 0));
    CHECK(mu_pick_preview_due(&sender, big, 100 + MU_PICK_PREVIEW_INTERVAL, 0));
    CHECK(!mu_pick_preview_due(&sender, big, 100 + 2 * MU_PICK_PREVIEW_INTERVAL, 0));
    CHECK(mu_pick_preview_due(&sender, wire, 100 + MU_PICK_PREVIEW_INTERVAL + 1, 1));
    /* Frame counters wrap. */
    memset(&sender, 0, sizeof sender);
    CHECK(mu_pick_preview_due(&sender, wire, 0xFFFFFFFEu, 0));
    CHECK(!mu_pick_preview_due(&sender, big, 2, 0));
    CHECK(mu_pick_preview_due(&sender, big, 4, 0));

    /* The read reply: valid slots round trip, invalid ones come back zero. */
    memset(slots, 0, sizeof slots);
    slots[0].valid = 1; slots[0].port = 0; slots[0].pick = p;
    slots[2].valid = 1; slots[2].port = 3; mu_pick_preview_clear(&slots[2].pick);
    slots[1].valid = 0; slots[1].port = 2; slots[1].pick = p;
    CHECK(mu_pick_preview_reply_encode(slots, reply, MU_PICK_PREVIEW_REPLY - 1) == 0);
    n = mu_pick_preview_reply_encode(slots, reply, sizeof reply);
    CHECK(n == MU_PICK_PREVIEW_REPLY);
    CHECK(mu_pick_preview_reply_decode(reply, n, back));
    CHECK(back[0].valid && back[0].port == 0 && memcmp(&back[0].pick, &p, sizeof p) == 0);
    CHECK(!back[1].valid && back[1].port == 0);
    CHECK(back[2].valid && back[2].port == 3 && back[2].pick.char_id == 0xFF);
    for (i = 0; i <= MU_PICK_PREVIEW_REPLY + 1; ++i) {
        reply[MU_PICK_PREVIEW_REPLY] = 0;
        CHECK(mu_pick_preview_reply_decode(reply, i, back) == (i == MU_PICK_PREVIEW_REPLY));
        if (i != MU_PICK_PREVIEW_REPLY) CHECK(!back[0].valid && !back[2].valid);
    }
    reply[0] = 2;
    CHECK(!mu_pick_preview_reply_decode(reply, n, back) && !back[0].valid);
    reply[0] = MU_PICK_PREVIEW_VERSION;
    reply[1 + 14 + 14 + 1] = 4;   /* slot 2's port out of range: only that slot drops */
    CHECK(mu_pick_preview_reply_decode(reply, n, back) && back[0].valid && !back[2].valid);
    reply[1 + 14 + 14 + 1] = 3;
    reply[1 + 2] = 9;             /* slot 0's version */
    CHECK(mu_pick_preview_reply_decode(reply, n, back) && !back[0].valid && back[2].valid);
    printf("PASS\n");
    return 0;
}
'''


def run(args):
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(' '.join(map(str, args)) + '\n' + result.stdout + result.stderr)
    return result.stdout


class PickPreviewCodec(unittest.TestCase):
    def test_codec_in_c_and_cxx(self):
        cc, cxx = shutil.which('cc'), shutil.which('c++')
        if not cc or not cxx:
            self.skipTest('needs C and C++ compilers')
        with tempfile.TemporaryDirectory(prefix='pick-preview-') as directory:
            work = Path(directory)
            (work / 'codec.c').write_text(CODEC)
            (work / 'codec.cpp').write_text(CODEC)
            run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                 '-I', str(ABI), str(work / 'codec.c'), '-o', str(work / 'c')])
            run([cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                 '-I', str(ABI), str(work / 'codec.cpp'), '-o', str(work / 'cxx')])
            self.assertIn('PASS', run([str(work / 'c')]))
            self.assertIn('PASS', run([str(work / 'cxx')]))


class PickPreviewWiring(unittest.TestCase):
    def test_ids_are_free_and_next_in_line(self):
        header = read(ABI / 'mu_pick_preview.h')
        self.assertIn('#define MU_PICK_PREVIEW_CAP 0x20', header)
        self.assertIn('#define MU_PICK_PREVIEW_MSG 0xE4', header)
        self.assertIn('#define MU_PICK_PREVIEW_COMMAND 0xED', header)
        self.assertIn('#define MU_SQUAD_CAP 0x10', read(ABI / 'mu_squad_wire.h'))
        net_h = read(ROOT / 'port/runtime/hle/slippi_net.h')
        ids = net_h.split('enum NetMsg : uint8_t {', 1)[1].split('};', 1)[0]
        self.assertIn('NP_MSG_MU_PICK_PREVIEW = MU_PICK_PREVIEW_MSG', ids)
        self.assertNotIn('0xE4', ids)
        # Not behind MU_SQUAD: every build sends and takes previews.
        self.assertIn('NP_MSG_MU_PICK_PREVIEW', without_squad(net_h))
        # 0xED is no other game/host command (EC is Squad Strike's, EE Maker's, EF the menu's).
        for path in ['port/runtime/hle/' + 'native_slippi_bridge.h', 'port/app/source_host.cpp',
                     'sourceport/game/shim/mu_slippi_menu.c']:
            self.assertNotRegex(read(ROOT / path), r'(?i)= 0xED\b|== 0xED\b|define \w+ 0xED\b', path)

    def test_build_caps_advertise_and_receipt_records_them(self):
        net = read(ROOT / 'port/runtime/hle/slippi_net.cpp')
        build = function(net, 'NetplayClient::SendBuild')
        self.assertIn('caps |= MU_PICK_PREVIEW_CAP;', build)
        self.assertLess(build.index('caps |= MU_PICK_PREVIEW_CAP;'), build.index('*p << caps;'))
        received = net[net.index('case NP_MSG_MU_BUILD:'):net.index('case NP_MSG_MU_EARLY_PAD:')]
        self.assertIn('remote_build_[pidx].pick_previews = (caps & MU_PICK_PREVIEW_CAP) != 0;', received)

    def test_sends_only_to_capable_connected_peers_on_change(self):
        net = read(ROOT / 'port/runtime/hle/slippi_net.cpp')
        send = function(net, 'NetplayClient::Send')
        gate = send[send.index('if (mid == NP_MSG_MU_PICK_PREVIEW)'):]
        gate = gate[:gate.index('}') + 1]
        self.assertIn('!remote_build_[i].pick_previews) continue;', gate)
        self.assertNotIn('UNSEQUENCED', gate)   # reliable channel 0, ordered with 0x82
        public = function(net, 'NetplayClient::SendPickPreview')
        self.assertIn('!= ConnectStatus::CONNECTED', public)
        queue = function(net, 'NetplayClient::QueuePickPreview')
        self.assertIn('if (!peers) return;', queue)
        self.assertIn('mu_pick_preview_due(&pick_sender_, bytes, frame, force)', queue)
        self.assertLess(queue.index('mu_pick_preview_due('), queue.index('SendAsync('))
        # pick_mutex_ is a leaf: its scope closes before SendAsync takes async_mutex_.
        locked = queue[queue.index('std::lock_guard<std::mutex> lk(pick_mutex_);'):]
        self.assertLess(locked.index('pick_sent_peers_ = peers;\n  }'), locked.index('SendAsync('))
        # A lock-in's preview precedes its 0x82.
        selections = function(net, 'NetplayClient::SetMatchSelections')
        self.assertLess(selections.index('SendLockedPickPreview();'), selections.index('WriteSelections('))

    def test_receipt_binds_the_enet_sender_and_clears(self):
        net = read(ROOT / 'port/runtime/hle/slippi_net.cpp')
        case = net[net.index('case NP_MSG_MU_PICK_PREVIEW:'):net.index('case NP_MSG_SLIPPI_CONN_SELECTED:')]
        self.assertIn('entry.first == peer && !entry.second.is_disconnected', case)
        self.assertIn('PlayerIdxFromPort(sender)', case)
        self.assertIn('mu_pick_preview_decode(packet.data() + 1, packet.size() - 1, &pick)', case)
        # The payload carries no port at all: the index comes from ENet's peer only.
        self.assertLess(case.index('sender = entry.second.player_idx'), case.index('mu_pick_preview_decode'))
        self.assertIn('for (bool& set : pick_remote_set_) set = false;', function(net, 'NetplayClient::StartSlippiGame'))
        self.assertIn('for (bool& set : pick_remote_set_) set = false;', function(net, 'NetplayClient::Disconnect'))
        read_back = function(net, 'NetplayClient::RemotePickPreview')
        self.assertIn('player_active_[pick_remote_port_[index]]', read_back)

    def test_command_route_and_payload_rule(self):
        bridge = read(ROOT / 'port/runtime/hle' / 'native_slippi_bridge.h')
        rule = bridge[bridge.index('if (cmd == MU_PICK_PREVIEW_COMMAND)'):]
        rule = rule[:rule.index(';') + 1]
        self.assertIn('payload_len == 1u + MU_PICK_PREVIEW_WIRE && payload[0] == MU_PICK_PREVIEW_OP_SEND', rule)
        self.assertIn('payload_len == 1 && payload[0] == MU_PICK_PREVIEW_OP_READ', rule)
        online = read(ROOT / 'port/runtime/hle/slippi_online.cpp')
        handle = function(online, 'handle_command')
        self.assertLess(handle.index('if (cmd == MU_PICK_PREVIEW_COMMAND) return pick_preview_command('),
                        handle.index('if (!valid_command_payload('))
        command = function(online, 'pick_preview_command')
        self.assertIn('g_netplay->SendPickPreview(pick, host::retrace_count())', command)
        self.assertIn('mu_pick_preview_reply_encode(slots', command)
        self.assertIn('cmd == MU_PICK_PREVIEW_COMMAND)', function(online, 'handle'))   # kept out of the trace

    def test_game_sends_every_css_frame_and_reads_beside_b3(self):
        css = read(GAME / 'shim/mu_slippi_css.c')
        self.assertIn('pick_preview_frame();', function(css, 'text_think'))
        frame = function(css, 'pick_preview_frame')
        self.assertIn('mu_css_pick_preview(0, port, &ck, &color);', frame)
        self.assertIn('dt.msrb.local_ready != 0', frame)
        self.assertIn('gmSquad_CssPreview(&ck, &color, lineup)', frame)
        self.assertNotIn('gmSquad', without_squad(frame))
        self.assertIn('mu_ak_mex_external(ck)', frame)
        menu = read(GAME / 'shim/mu_slippi_menu.c')
        load = function(menu, 'mu_slippi_load_match_state')
        self.assertLess(load.index('parse_match_state(&last_match, msrb);'), load.index('load_pick_previews(&last_match);'))
        self.assertIn('MU_PICK_PREVIEW_OP_READ', function(menu, 'load_pick_previews'))
        self.assertIn('mu_slippi_cmd(MU_PICK_PREVIEW_COMMAND, b, sizeof b', function(menu, 'mu_pick_preview_send'))
        native = read(GAME / 'include/mu_native.h')
        state = native[native.index('typedef struct MuMatchState {'):native.index('} MuMatchState;')]
        # After the MSRB copy (Slippi 3.7.0's 1030 bytes): the MSRB itself is unchanged.
        self.assertLess(state.index('unsigned char raw[1030];'), state.index('MuOpponentPick opp_picks[3];'))
        pick = native[native.index('typedef struct MuOpponentPick {'):native.index('} MuOpponentPick;')]
        for field in ['valid, port', 'char_id', 'color', 'team_id', 'squad_count', 'kinds[3]', 'colors[3]', 'locked']:
            self.assertIn(field, pick)

    def test_squad_accessor_is_appended_inside_the_optional_block(self):
        css = read(GAME / 'shim/mu_squad_css.c')
        accessor = function(css, 'gmSquad_CssPreview')
        self.assertIn('gmSquad_Lineup(0, *coin_ck, *coin_color, out)', accessor)
        tail = css[css.index('int gmSquad_CssPreview('):]
        self.assertEqual(tail.rstrip().splitlines()[-1], '#endif')
        self.assertNotIn('gmSquad_CssPreview', without_squad(css))
        transfer = read(GAME / 'include/mu_css_transfer.inc')
        self.assertIn('void mu_css_pick_preview(int door, int port', transfer)
        self.assertNotIn('Squad', without_squad(transfer))


if __name__ == '__main__':
    unittest.main()
