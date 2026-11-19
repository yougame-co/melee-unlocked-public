"""The match setup hash (NP_MSG_MU_SETUP_HASH): its codec and decisions, compiled and run, and the
wiring that holds a match until every peer's hash agrees and refuses one that does not."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from squad_wiring_test import ROOT, function, read

ABI = ROOT / 'port/runtime/abi/mu_setup_hash.h'
NET_H = ROOT / 'port/runtime/hle/slippi_net.h'
NET = ROOT / 'port/runtime/hle/slippi_net.cpp'
ONLINE = ROOT / 'port/runtime/hle/slippi_online.cpp'

CODEC_TEST = r'''
#include "mu_setup_hash.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)
int main(void)
{
    MuSetupHash a = {2, 0x0123456789ABCDEFull, 0xFEDCBA9876543210ull}, b, peer;
    unsigned char wire[MU_SETUP_HASH_WIRE + 4];
    const unsigned char expect[MU_SETUP_HASH_WIRE] = {'M', 'U', 'H', MU_SETUP_HASH_VERSION, 2,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10};
    uint64_t h;
    CHECK(mu_setup_hash_encode(&a, wire, MU_SETUP_HASH_WIRE - 1) == 0);
    CHECK(mu_setup_hash_encode(&a, wire, sizeof wire) == MU_SETUP_HASH_WIRE);
    CHECK(memcmp(wire, expect, sizeof expect) == 0);   /* big-endian on every machine */
    memset(&b, 0, sizeof b);
    CHECK(mu_setup_hash_decode(wire, MU_SETUP_HASH_WIRE, &b));
    CHECK(b.port == 2 && b.inputs == a.inputs && b.setup == a.setup);
    CHECK(!mu_setup_hash_decode(wire, MU_SETUP_HASH_WIRE - 1, &b));
    CHECK(!mu_setup_hash_decode(wire, MU_SETUP_HASH_WIRE + 1, &b));
    CHECK(!mu_setup_hash_decode(wire, 0, &b));
    CHECK(!mu_setup_hash_decode(NULL, MU_SETUP_HASH_WIRE, &b));
    wire[0] = 'X';
    CHECK(!mu_setup_hash_decode(wire, MU_SETUP_HASH_WIRE, &b));
    wire[0] = 'M';
    wire[3] = MU_SETUP_HASH_VERSION + 1;
    CHECK(!mu_setup_hash_decode(wire, MU_SETUP_HASH_WIRE, &b));

    /* FNV-1a 64: the published test vectors, and the length prefix keeps fields apart. */
    CHECK(mu_fnv64(MU_FNV64_OFFSET, "", 0) == 0xCBF29CE484222325ull);
    CHECK(mu_fnv64(MU_FNV64_OFFSET, "a", 1) == 0xAF63DC4C8601EC8Cull);
    CHECK(mu_fnv64(MU_FNV64_OFFSET, "foobar", 6) == 0x85944171F73967E8ull);
    CHECK(mu_fnv64_bytes(mu_fnv64_bytes(MU_FNV64_OFFSET, "ab", 2), "c", 1) !=
          mu_fnv64_bytes(mu_fnv64_bytes(MU_FNV64_OFFSET, "a", 1), "bc", 2));
    h = mu_fnv64_u16(MU_FNV64_OFFSET, 0x1234);
    CHECK(h == mu_fnv64(MU_FNV64_OFFSET, "\x12\x34", 2));
    h = mu_fnv64_u32(MU_FNV64_OFFSET, 0x12345678u);
    CHECK(h == mu_fnv64(MU_FNV64_OFFSET, "\x12\x34\x56\x78", 4));

    /* Decisions: no cap is skipped, nothing yet or other inputs wait, same inputs decide. */
    peer = a;
    CHECK(mu_setup_hash_decide(0, 1, &a, &peer) == MU_SETUP_HASH_SKIP);
    CHECK(mu_setup_hash_decide(1, 0, &a, &peer) == MU_SETUP_HASH_WAIT);
    CHECK(mu_setup_hash_decide(1, 1, &a, &peer) == MU_SETUP_HASH_AGREE);
    peer.setup ^= 1;
    CHECK(mu_setup_hash_decide(1, 1, &a, &peer) == MU_SETUP_HASH_MISMATCH);
    peer.inputs ^= 1;   /* built from selections we have not seen yet: never a refusal */
    CHECK(mu_setup_hash_decide(1, 1, &a, &peer) == MU_SETUP_HASH_WAIT);
    CHECK(mu_setup_hash_decide(1, 1, NULL, &peer) == MU_SETUP_HASH_WAIT);
    printf("ok\n");
    return 0;
}
'''


class SetupHashCodec(unittest.TestCase):
    def test_codec_and_decisions_compile_and_pass(self):
        for compiler, suffix, flags in [('cc', '.c', ['-std=c99', '-pedantic']),
                                        ('c++', '.cpp', ['-std=c++17'])]:
            with tempfile.TemporaryDirectory() as tmp:
                src = Path(tmp) / ('t' + suffix)
                src.write_text(CODEC_TEST)
                exe = Path(tmp) / 't'
                build = subprocess.run([compiler, *flags, '-Wall', '-Werror', '-I', str(ABI.parent), str(src),
                                        '-o', str(exe)], capture_output=True, text=True)
                self.assertEqual(build.returncode, 0, build.stderr)
                run = subprocess.run([str(exe)], capture_output=True, text=True)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertEqual(run.stdout.strip(), 'ok')

    def test_decide_has_no_clock(self):
        text = read(ABI)
        decide = text.split('mu_setup_hash_decide', 1)[1].split('\n}\n', 1)[0]
        self.assertIn('MU_SETUP_HASH_MISMATCH', decide)
        self.assertNotRegex(decide, r'time|clock|ms\b')


# A 4-player match's setup, built twice from the same inputs on the block the game keeps (the
# shipped initial block, read from slippi_online.cpp). The first build used to count costume
# indices before P3/P4 were made human ports, so it hashed differently from every later one.
PORTS_TEST = r'''
#include "mu_setup_hash.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)
static unsigned char block[] = { BLOCK };
static void build(const unsigned char (*seats)[3], unsigned count, int teams)
{
    unsigned i;
    for (i = 0; i < count; ++i) {   /* the builder's per-player writes, then the ports */
        block[0x60 + i * 0x24] = seats[i][0];
        block[0x63 + i * 0x24] = seats[i][1];
        block[0x67 + i * 0x24] = 0;
        block[0x69 + i * 0x24] = seats[i][2];
    }
    mu_setup_ports(block, sizeof block, count - 1, teams);
}
int main(void)
{
    /* The field's Doubles setup: P1 P2 char 11 colour 0 team 0, P3 P4 char 20 colour 2 team 1. */
    static const unsigned char doubles[4][3] = {{11, 0, 0}, {11, 0, 0}, {20, 2, 1}, {20, 2, 1}};
    static const unsigned char singles[2][3] = {{11, 0, 0}, {11, 0, 1}};
    static const unsigned char climbers[4][3] = {{0x12, 0, 0}, {0x13, 0, 0}, {0x12, 1, 1}, {0x0E, 0, 1}};
    unsigned char first[sizeof block];
    CHECK(sizeof block >= 0x60 + 4 * 0x24);
    CHECK(block[0x61 + 2 * 0x24] == 3 && block[0x61 + 3 * 0x24] == 3);   /* nobody at P3/P4 at boot */
    build(doubles, 4, 1);
    memcpy(first, block, sizeof block);
    build(doubles, 4, 1);
    CHECK(memcmp(first, block, sizeof block) == 0);   /* the same inputs build the same block */
    CHECK(block[0x61 + 2 * 0x24] == 0 && block[0x61 + 3 * 0x24] == 0);
    CHECK(block[0x67] == 0 && block[0x67 + 0x24] == 1 && block[0x67 + 2 * 0x24] == 0 && block[0x67 + 3 * 0x24] == 1);

    /* A 1v1 after it: P3/P4 are empty again and their old records count for nobody. */
    build(singles, 2, 0);
    CHECK(block[0x61 + 2 * 0x24] == 3 && block[0x61 + 3 * 0x24] == 3);
    CHECK(block[0x67] == 0 && block[0x67 + 0x24] == 1);   /* same character, same colour */
    memcpy(first, block, sizeof block);
    build(singles, 2, 0);
    CHECK(memcmp(first, block, sizeof block) == 0);

    /* Teams count by team, Ice Climbers as Popo; then the 4-player match again, from the 1v1's block. */
    build(climbers, 4, 1);
    CHECK(block[0x67] == 0 && block[0x67 + 0x24] == 1 && block[0x67 + 2 * 0x24] == 0 && block[0x67 + 3 * 0x24] == 0);
    build(doubles, 4, 1);
    memcpy(first, block, sizeof block);
    build(doubles, 4, 1);
    CHECK(memcmp(first, block, sizeof block) == 0);
    mu_setup_ports(NULL, 0, 3, 1);   /* no block: nothing */
    printf("ok\n");
    return 0;
}
'''

# The field's sequence on one seat: every machine built setup 1, sent it, then built setup 2 from
# the same inputs a frame later; a seat holding a peer's setup 1 compared it with its own setup 2.
SETTLE_TEST = r'''
#include "mu_setup_hash.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)
int main(void)
{
    const uint64_t inputs = 0x7072ed0d152cd2d8ull, one = 0x3238db2e2a05243bull, two = 0xc1288f546a99aaa9ull;
    MuSetupHash local = {0, inputs, one}, peer = {1, inputs, one};
    MuSetupHashPending pending = {0, 0, 0, 0};
    uint64_t t = 1000000;
    CHECK(mu_setup_hash_decide(1, 1, &local, &peer) == MU_SETUP_HASH_AGREE);
    local.setup = two;   /* this side's next build; the peer's is still on the wire */
    CHECK(mu_setup_hash_decide(1, 1, &local, &peer) == MU_SETUP_HASH_MISMATCH);
    CHECK(!mu_setup_hash_mismatch_final(&pending, &local, &peer, t));
    CHECK(!mu_setup_hash_mismatch_final(&pending, &local, &peer, t + 17));
    peer.setup = two;   /* it arrives: the match starts */
    CHECK(mu_setup_hash_decide(1, 1, &local, &peer) == MU_SETUP_HASH_AGREE);
    pending.open = 0;   /* the gate resets it on any other decision */

    /* A real disagreement still refuses, once it has stood. */
    peer.setup = one;
    CHECK(!mu_setup_hash_mismatch_final(&pending, &local, &peer, t + 100));
    CHECK(!mu_setup_hash_mismatch_final(&pending, &local, &peer, t + 100 + MU_SETUP_HASH_SETTLE_MS - 1));
    CHECK(mu_setup_hash_mismatch_final(&pending, &local, &peer, t + 100 + MU_SETUP_HASH_SETTLE_MS));
    /* Either side's next hash restarts the wait. */
    peer.setup = 0x1234;
    CHECK(!mu_setup_hash_mismatch_final(&pending, &local, &peer, t + 5000));
    local.setup = 0x5678;
    CHECK(!mu_setup_hash_mismatch_final(&pending, &local, &peer, t + 5000 + MU_SETUP_HASH_SETTLE_MS));
    CHECK(mu_setup_hash_mismatch_final(&pending, &local, &peer, t + 5000 + 2 * MU_SETUP_HASH_SETTLE_MS));
    CHECK(MU_SETUP_HASH_SETTLE_MS < 8000);   /* inside the connect timeout */
    CHECK(mu_setup_hash_mismatch_final(NULL, &local, &peer, t));
    printf("ok\n");
    return 0;
}
'''


# 11-18 ranked: both seats agreed on the lock-in setup, struck stages for two minutes, then the seat
# that finished first sent its overwrite's hash. The other seat, one step behind, counted its wait
# from its lock-in setup and dropped the peer on the spot. A peer's newer hash restarts the wait.
WAIT_TEST = r'''
#include "mu_setup_hash.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)
int main(void)
{
    const uint64_t T = 8000;
    MuSetupHash local = {1, 0x11, 0xA1}, peer = {0, 0x11, 0xA1};
    MuSetupHashWait wait = {0, 0, 0, 0};
    /* Lock-in: both built the same setup and agree (the gate only reads the clock while waiting). */
    mu_setup_hash_wait_over(&wait, 0, 1, &peer, 50, T);
    CHECK(mu_setup_hash_decide(1, 1, &local, &peer) == MU_SETUP_HASH_AGREE);
    mu_setup_hash_wait_over(&wait, 0, 1, &peer, 60000, T);   /* noted on every call, agreed or not */
    /* Two minutes of strikes later the faster seat's overwrite hash arrives: other inputs, a wait. */
    peer.inputs = 0x22; peer.setup = 0xB2;
    CHECK(mu_setup_hash_decide(1, 1, &local, &peer) == MU_SETUP_HASH_WAIT);
    CHECK(120000 - 0 >= T);                                          /* 122.19's clock: dropped here */
    CHECK(!mu_setup_hash_wait_over(&wait, 0, 1, &peer, 120000, T));   /* counted from its arrival */
    CHECK(!mu_setup_hash_wait_over(&wait, 0, 1, &peer, 120000 + T - 1, T));
    CHECK(mu_setup_hash_wait_over(&wait, 0, 1, &peer, 120000 + T, T));   /* a real stall still ends */
    /* This side's own newer setup counts too: whichever came last. */
    CHECK(!mu_setup_hash_wait_over(&wait, 125000, 1, &peer, 125000 + T - 1, T));
    CHECK(mu_setup_hash_wait_over(&wait, 125000, 1, &peer, 125000 + T, T));
    /* No hash ever from the peer: the local setup's clock alone, as before. */
    {
        MuSetupHashWait none = {0, 0, 0, 0};
        CHECK(!mu_setup_hash_wait_over(&none, 1000, 0, &peer, 1000 + T - 1, T));
        CHECK(mu_setup_hash_wait_over(&none, 1000, 0, &peer, 1000 + T, T));
        CHECK(mu_setup_hash_wait_over(NULL, 1000, 1, &peer, 1000 + T, T));
    }
    printf("ok\n");
    return 0;
}
'''


def shipped_block():
    body = read(ONLINE).split('static std::vector<uint8_t> block = {', 1)[1].split('};', 1)[0]
    return ', '.join(re.findall(r'0x[0-9A-Fa-f]+', body))


def compile_and_run(case, source):
    for compiler, suffix, flags in [('cc', '.c', ['-std=c99', '-pedantic']),
                                    ('c++', '.cpp', ['-std=c++17'])]:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / ('t' + suffix)
            src.write_text(source)
            exe = Path(tmp) / 't'
            build = subprocess.run([compiler, *flags, '-Wall', '-Werror', '-I', str(ABI.parent), str(src),
                                    '-o', str(exe)], capture_output=True, text=True)
            case.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            case.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            case.assertEqual(run.stdout.strip(), 'ok')


class SetupIsAFunctionOfItsInputs(unittest.TestCase):
    def test_a_four_player_setup_builds_the_same_every_time(self):
        compile_and_run(self, PORTS_TEST.replace('BLOCK', shipped_block()))

    def test_a_peer_hash_that_is_already_replaced_never_refuses(self):
        compile_and_run(self, SETTLE_TEST)

    def test_a_peer_one_step_ahead_is_not_a_lost_peer(self):
        compile_and_run(self, WAIT_TEST)


class SetupHashWiring(unittest.TestCase):
    def test_message_id_and_cap_are_free(self):
        header = read(NET_H)
        abi = read(ABI)
        self.assertIn('#define MU_SETUP_HASH_MSG 0xE6', abi)
        self.assertIn('#define MU_SETUP_HASH_CAP 0x40', abi)
        self.assertIn('NP_MSG_MU_SETUP_HASH = 0xE6,', header)
        self.assertIn('static_assert(NP_MSG_MU_SETUP_HASH == MU_SETUP_HASH_MSG', header)
        ids = re.findall(r'NP_MSG_\w+ = (0x[0-9A-Fa-f]+)', header)
        self.assertEqual(len(ids), len(set(ids)), ids)
        others = [int(v, 16) for v in ids if v.upper() != '0XE6']
        self.assertGreaterEqual(0xE6, max(others) + 3)
        # 0x20 stays spare; no other cap uses 0x40.
        net = read(NET)
        caps = re.findall(r'caps \|= (0x[0-9A-Fa-f]+|MU_\w+_CAP)', net)
        self.assertIn('MU_SETUP_HASH_CAP', caps)
        self.assertNotIn('0x40', caps)
        self.assertNotIn('0x20', caps)

    def test_build_advertises_and_receipt_records_the_cap(self):
        net = read(NET)
        build = function(net, 'NetplayClient::SendBuild')
        self.assertIn('caps |= MU_SETUP_HASH_CAP', build)
        self.assertIn('MELEE_TEST_NO_SETUP_HASH', build)
        self.assertIn('remote_build_[pidx].setup_hash = (caps & MU_SETUP_HASH_CAP) != 0;', net)
        self.assertIn('has no setup hash (not checked)', net)

    def test_receipt_checks_length_and_sender(self):
        net = read(NET)
        case = net.split('case NP_MSG_MU_SETUP_HASH: {', 1)[1].split('\n    case ', 1)[0]
        self.assertIn('mu_setup_hash_decode(', case)
        self.assertIn('packet.size() - 1', case)
        self.assertIn('PlayerIdxFromPort(h.port)', case)
        self.assertIn('active_connections_', case)   # the hash names the ENet peer that sent it
        self.assertIn('setup_hash_[pidx] = {true, h};', case)
        send = function(net, 'NetplayClient::SendSetupHash')
        self.assertIn('mu_setup_hash_encode(', send)
        self.assertIn('SendAsync(', send)   # reliable channel 0, every peer

    def test_connect_timeout_is_shared(self):
        net = read(NET)
        self.assertIn('constexpr uint64_t kConnectTimeoutMs = 8000;', read(NET_H))
        thread = net.split('void NetplayClient::ThreadFunc()', 1)[1].split('\nvoid NetplayClient::', 1)[0]
        self.assertIn('const uint64_t timeout = kConnectTimeoutMs;', thread)

    def test_gate_holds_ready_and_refuses_a_mismatch(self):
        online = read(ONLINE)
        gate = function(online, 'setup_hash_gate')
        self.assertIn('mu_setup_hash_decide(', gate)
        self.assertIn('*why = MU_SETUP_HASH_TEXT;', gate)
        self.assertIn('kConnectTimeoutMs', gate)
        self.assertIn('why->clear();', gate)   # a missing hash is a plain disconnect
        self.assertIn('setup hash %016llx local, %016llx remote[%d]', gate)
        self.assertIn('SendSetupHash(inputs, setup)', gate)
        state = function(online, 'prepare_online_match_state')
        at = state.index('setup_hash_gate(')
        # The block is final (items written) before it is hashed, and the hold comes before the RNG
        # offset and the rest of the reply.
        self.assertLess(state.index('block[0x23 + i] = (uint8_t)(items'), at)
        self.assertLess(at, state.index('append_u32(q, rng_offset);'))
        refusal = state[at:state.index('append_u32(q, rng_offset);')]
        for step in ['g_netplay->ForceDisconnect();', 'cleanup_connection();', 'g_forced_error = why;',
                     'prepare_online_match_state(q);', 'return;', 'q[2] = 0;']:
            self.assertIn(step, refusal)
        self.assertLess(refusal.index('cleanup_connection();'), refusal.index('g_forced_error = why;'))
        # Inputs are what was sent, before the game's overwrites change the selections.
        self.assertLess(state.index('sent_selections.push_back(*o);'),
                        state.index('ordered[i]->character_id = g_overwrite_selections[i].character_id;'))
        self.assertIn('g_setup_hash = {};', function(online, 'cleanup_connection'))
        # The ports (P3/P4's slot types, then costume indices) come from this build alone, before the hash.
        ports = state.index('mu_setup_ports(block.data(), block.size(), uint8_t(ordered.size() - 1),')
        self.assertLess(ports, at)
        self.assertNotIn('color_counts', state)
        self.assertNotIn('block[0x61 + 2 * 0x24] =', state)
        # A mismatch refuses only once it has stood; a pending one is a wait, and still refuses at the timeout.
        self.assertLess(gate.index('mu_setup_hash_mismatch_final(&g_setup_hash.pending[i]'), gate.index('*why = MU_SETUP_HASH_TEXT;'))
        self.assertIn('if (decision != MU_SETUP_HASH_MISMATCH) g_setup_hash.pending[i].open = 0;', gate)
        self.assertIn('decision == MU_SETUP_HASH_WAIT || decision == MU_SETUP_HASH_MISMATCH', gate)
        # The connect timeout counts from this side's setup or the peer's newest hash, whichever is later
        # (mu_setup_hash_wait_over), noted for every peer on every call; never from the setup alone.
        self.assertIn('mu_setup_hash_wait_over(&g_setup_hash.wait[i], g_setup_hash.since_ms, peer.received,', gate)
        self.assertIn('MuSetupHashWait wait[REMOTE_PLAYER_MAX]', online)
        self.assertNotIn('time_ms() - g_setup_hash.since_ms >= kConnectTimeoutMs', gate)
        self.assertLess(gate.index('mu_setup_hash_wait_over('), gate.index('if (decision != MU_SETUP_HASH_MISMATCH)'))
        self.assertIn('wait_over = wait_over || over;', gate)
        timeout = gate[gate.index('if (wait_over) {'):]
        self.assertLess(timeout.index('*why = MU_SETUP_HASH_TEXT;'), timeout.index('why->clear();'))
        self.assertIn("Match setup didn't match your opponent's. Match not started.", read(ABI))

    def test_setup_hash_covers_the_match(self):
        online = read(ONLINE)
        setup = function(online, 'setup_block_hash')
        for part in ['g_last_search.mode', 'players', '0x61, 0x60, 0x63, 0x67, 0x69', 'block[0xE]',
                     'mu_fnv64_u32(h, rng)', 'alt_stage', 'mu_fnv64_bytes(h, block.data(), block.size())',
                     'squad.data()']:
            self.assertIn(part, setup)
        # An empty port keeps the last match's record in the kept block: only its slot type counts.
        self.assertIn('setup_block_hash(std::vector<uint8_t> block,', setup)   # a copy, never the game's block
        self.assertLess(setup.index('std::fill(block.begin() + record'), setup.index('mu_fnv64_bytes(h, block.data()'))
        inputs = function(online, 'setup_inputs_hash')
        for part in ['is_character_selected', 'is_stage_selected', 'rng_offset', 'g_overwrite_selections',
                     'room_teams', 'squad.data()']:
            self.assertIn(part, inputs)

    def test_squad_budget_identity_is_setup_not_inputs(self):
        online = read(ONLINE)
        setup = function(online, 'setup_block_hash')
        self.assertIn('const std::string& squad_budget', setup)
        live = setup[setup.index('if (!squad.empty()) {'):]
        for part in ['squad_budget.data(), squad_budget.size()', 'MU_SQUAD_ONLINE_ALLM', 'MU_SQUAD_ONLINE_ALLA']:
            self.assertIn(part, live)
        self.assertNotIn('budget', function(online, 'setup_inputs_hash'))
        state = function(online, 'prepare_online_match_state')
        self.assertIn('setup_budget = g_squad_budget;', state)
        self.assertLess(state.index('setup_squad.assign(bytes, bytes + size);'), state.index('setup_budget = g_squad_budget;'))
        # Then the Friends lobby's frozen-setup digest (0 for every other match).
        self.assertIn('setup_squad, setup_budget, setup_lobby), &why);', state)
        self.assertIn('#define MU_SETUP_HASH_VERSION 1', read(ABI))   # setup bytes only: no wire change
        self.assertIn('squad budget identity', read(ABI))


SETUP_BLOCK_TEST = r'''
#include "mu_setup_hash.h"
#include "mu_squad_wire.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %d %s\n", __LINE__, #x); return 1; } } while (0)
constexpr int PLAYER_COUNT_MAX = 4;
struct { int mode = 1; } g_last_search;
@SETUP_BLOCK_HASH@
int main()
{
    std::vector<uint8_t> block(0x138, 0), squad = {MU_SQUAD_VERSION, 3, 3, 2, 0, 9, 1, 20, 0, 3, 1, 0, 8, 2, 12, 0};
    const std::string a(64, 'a'), b = std::string(63, 'a') + 'b';
    for (int i = 0; i < PLAYER_COUNT_MAX; ++i) block[0x61 + i * 0x24] = i < 2 ? 0 : 3;
    const uint64_t live_a = setup_block_hash(block, 2, 7, 0, squad, a);
    CHECK(live_a == setup_block_hash(block, 2, 7, 0, squad, a));
    CHECK(live_a != setup_block_hash(block, 2, 7, 0, squad, b));    /* other disc budget identity */
    CHECK(live_a != setup_block_hash(block, 2, 7, 0, squad, ""));   /* no identity at all */
    /* Not live: no squad bytes, and the identity is not part of the match. */
    CHECK(setup_block_hash(block, 2, 7, 0, {}, a) == setup_block_hash(block, 2, 7, 0, {}, b));
    CHECK(setup_block_hash(block, 2, 7, 0, {}, a) != live_a);
    /* The same inputs with another setup: a mismatch that refuses, never a wait. */
    MuSetupHash local = {0, 0x1234, live_a}, peer = {1, 0x1234, setup_block_hash(block, 2, 7, 0, squad, b)};
    CHECK(mu_setup_hash_decide(1, 1, &local, &peer) == MU_SETUP_HASH_MISMATCH);
    peer.setup = live_a;
    CHECK(mu_setup_hash_decide(1, 1, &local, &peer) == MU_SETUP_HASH_AGREE);
    /* The Friends lobby's CPUs (slot type 1): their whole record is the match's. Another level,
     * character or colour changes the hash; an empty port's leftovers still do not. */
    std::vector<uint8_t> cpu = block;
    cpu[0x61 + 2 * 0x24] = 1; cpu[0x60 + 2 * 0x24] = 9; cpu[0x6F + 2 * 0x24] = 9;
    const uint64_t with_cpu = setup_block_hash(cpu, 2, 7, 0, {}, "");
    CHECK(with_cpu != setup_block_hash(block, 2, 7, 0, {}, ""));
    std::vector<uint8_t> other = cpu;
    other[0x6F + 2 * 0x24] = 3;                                   /* level 3, not 9 */
    CHECK(setup_block_hash(other, 2, 7, 0, {}, "") != with_cpu);
    other = cpu; other[0x60 + 2 * 0x24] = 10;                     /* another character */
    CHECK(setup_block_hash(other, 2, 7, 0, {}, "") != with_cpu);
    other = cpu; other[0x63 + 2 * 0x24] = 2;                      /* another colour */
    CHECK(setup_block_hash(other, 2, 7, 0, {}, "") != with_cpu);
    other = cpu; other[0x60 + 3 * 0x24] = 22; other[0x6F + 3 * 0x24] = 7;   /* P4 is empty: leftovers */
    CHECK(setup_block_hash(other, 2, 7, 0, {}, "") == with_cpu);
    /* The Friends lobby's frozen-setup digest: 0 (every other match) is the hash as before; two
     * different frozen setups for one match differ. */
    CHECK(setup_block_hash(cpu, 2, 7, 0, {}, "", 0) == with_cpu);
    CHECK(setup_block_hash(cpu, 2, 7, 0, {}, "", 0x1111) != with_cpu);
    CHECK(setup_block_hash(cpu, 2, 7, 0, {}, "", 0x1111) != setup_block_hash(cpu, 2, 7, 0, {}, "", 0x2222));
    std::printf("ok\n");
    return 0;
}
'''


class SetupBlockHashBehaviour(unittest.TestCase):
    def test_cpu_records_are_hashed(self):
        self.assertIn('mu_setup_record_hashed(block[record + 1])', function(read(ONLINE), 'setup_block_hash'))

    def test_squad_budget_changes_the_setup_hash(self):
        body = function(read(ONLINE), 'setup_block_hash')
        source = SETUP_BLOCK_TEST.replace('@SETUP_BLOCK_HASH@', body)
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / 't.cpp'
            src.write_text(source)
            exe = Path(tmp) / 't'
            build = subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', '-DMU_SQUAD=1', '-I', str(ABI.parent),
                                    str(src), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertEqual(run.stdout.strip(), 'ok')


# The Friends lobby's costume counts (the select's COSTUMES): every machine clamps each port's colour to
# its build's count before the hash (lobby::apply), so two machines with the same counts hash the same
# block, and a machine whose counts clamp a port's colour differently refuses the match.
COSTUME_HASH_TEST = r'''
#include "yougame_lobby_setup.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %d %s\n", __LINE__, #x); return 1; } } while (0)
constexpr int PLAYER_COUNT_MAX = 4;
struct { int mode = 1; } g_last_search;
@SETUP_BLOCK_HASH@
using namespace yougame::lobby;
static uint64_t hash_with(const Plan& p, const uint8_t* counts)
{
    std::vector<uint8_t> block(0x138, 0);
    if (!apply(p, block.data(), block.size(), 0, counts)) return 0;
    return setup_block_hash(block, 2, 7, 0, {}, "", 0x1234);
}
int main()
{
    json seats = json::array();
    seats.push_back({{"seat", 0}, {"port", 0}, {"kind", "human"}, {"id", "a"}, {"name", "A"}, {"char", 2}, {"color", 3}, {"team", 0}});
    seats.push_back({{"seat", 1}, {"port", 1}, {"kind", "human"}, {"id", "b"}, {"name", "B"}, {"char", 9}, {"color", 1}, {"team", 0}});
    const json frame = {{"t", "yg.mu.lobby"}, {"v", 1}, {"op", "setup"}, {"rev", 3}, {"matchId", "m-1"}, {"seed", 7},
        {"seats", seats}, {"rules", {{"teams", false}, {"friendlyFire", false}, {"mode", "stock"}, {"stocks", 4}, {"minutes", 8}, {"pause", true}}},
        {"stage", 31}};
    const Incoming in = decode(frame);
    CHECK(in.op == Op::Setup && !in.why);
    const Plan p = plan(in.setup, "a");
    CHECK(p.ok);
    uint8_t mine[MU_LOBBY_CHARS], theirs[MU_LOBBY_CHARS], fewer[MU_LOBBY_CHARS];
    for (unsigned ch = 0; ch < MU_LOBBY_CHARS; ++ch) mine[ch] = theirs[ch] = fewer[ch] = 8;   /* m-ex: 8 each */
    fewer[2] = 3;                                         /* their Fox has 3: colour 3 plays as 2, its last */
    const uint64_t h = hash_with(p, mine);
    CHECK(h != 0);
    CHECK(h == hash_with(p, theirs));                     /* same counts: same hash */
    CHECK(h != hash_with(p, fewer));                      /* different counts: the match is refused */
    CHECK(hash_with(p, fewer) == hash_with(p, fewer));
    {
        std::vector<uint8_t> block(0x138, 0);
        CHECK(apply(p, block.data(), block.size(), 0, fewer) && block[0x60 + 3] == 2);
        uint8_t first[MU_LOBBY_CHARS];
        for (unsigned ch = 0; ch < MU_LOBBY_CHARS; ++ch) first[ch] = fewer[ch];
        first[2] = 1;                                     /* one costume: 0 is its last */
        CHECK(apply(p, block.data(), block.size(), 0, first) && block[0x60 + 3] == 0);
        CHECK(hash_with(p, first) != hash_with(p, fewer));
    }
    std::printf("ok\n");
    return 0;
}
'''


class CostumeCountsInTheSetupHash(unittest.TestCase):
    def test_same_counts_same_hash_different_counts_differ(self):
        body = function(read(ONLINE), 'setup_block_hash')
        source = COSTUME_HASH_TEST.replace('@SETUP_BLOCK_HASH@', body)
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / 't.cpp'
            src.write_text(source)
            exe = Path(tmp) / 't'
            build = subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', '-I', str(ROOT / 'port/runtime/hle'),
                                    '-I', str(ABI.parent), '-I', str(ROOT / 'port/third_party'), str(src), '-o', str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertEqual(run.stdout.strip(), 'ok')


if __name__ == '__main__':
    unittest.main()
