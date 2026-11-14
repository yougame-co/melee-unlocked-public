"""Optional Squad Strike: guarded integration and production C behavioral fixtures."""
from pathlib import Path
import hashlib
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
GAME = ROOT / 'sourceport/game'
DECOMP = Path(os.environ.get('MELEE_DECOMP_DIR', str(ROOT / 'sourceport/extern/melee'))) / 'src/melee'
NEEDS_DECOMP = unittest.skipUnless((DECOMP / 'lb/lbheap.c').is_file(),
                                   'needs the patched decomp checkout (sourceport/extern/melee)')
CREDIT = ("Based on Squad Strike by Chris (x9x9x9x9x9), "
          "https://github.com/x9x9x9x9x9/squad-strike v1.0.0, "
          "used with the author's permission (11-03-2026)")


def read(path):
    return path.read_text()


def function(source, name):
    match = re.search(r'(?m)^[\w :*&<>]+\b' + re.escape(name) + r'\([^;]*?\)\s*\{', source)
    assert match, name
    at = source.index('{', match.start())
    depth = 1
    end = at + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


def without_squad(source):
    """Evaluate just the optional group; leave unrelated preprocessor text intact."""
    result, stack = [], []
    for line in source.splitlines(keepends=True):
        if re.match(r'#if(?:def|ndef)?\b', line):
            known = bool(re.match(r'#if MU_SQUAD\s*$', line))
            if not stack or all(enabled for enabled, _ in stack):
                if not known:
                    result.append(line)
            stack.append((not known, known))
        elif stack and line.startswith('#else'):
            enabled, known = stack[-1]
            if known:
                stack[-1] = (not enabled, known)
            elif all(value for value, _ in stack):
                result.append(line)
        elif stack and line.startswith('#endif'):
            _, known = stack.pop()
            if not known and all(value for value, _ in stack):
                result.append(line)
        # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
        elif re.fullmatch(r'#line [0-9]+ /\* Preserve ee1088f OFF debug declaration locations\. \*/\s*', line):
            continue  # location metadata; executable OFF bodies are still pinned below
        elif all(value for value, _ in stack):
            result.append(line)
    assert not stack
    return ''.join(result)


def production_fixture():
    header = read(GAME / 'include/mu_squad.h')
    source = read(GAME / 'shim/mu_squad.c')
    defines = '\n'.join(x for x in header.splitlines() if x.startswith('#define GMSQUAD_'))
    member = re.search(r'typedef struct GmSquadMember \{.*?} GmSquadMember;', header, re.S).group()
    state = source[source.index('/* Squad members after'):source.index('bool gmSquad_ModeAllowed')]
    names = ['gmSquad_Live', 'mu_squad_main_heap', 'mu_squad_animation_heap', 'mu_squad_sound_heap',
             'gmSquad_StockRule', 'squad_fixed_budget', 'squad_budget_file', 'squad_stage_bytes', 'gmSquad_Budget',
             'gmSquad_Count', 'gmSquad_StocksFor', 'gmSquad_Any', 'gmSquad_Clear', 'gmSquad_TrackCoin',
             'gmSquad_Lineup', 'gmSquad_ClearAll', 'gmSquad_Key', 'squad_console_rule', 'gmSquad_AddPreloads',
             'gmSquad_Disable', 'squad_trim', 'gmSquad_TrimOnline', 'squad_tint_start', 'gmSquad_OnEnterVs',
             'gmSquad_ApplyLiveStart', 'gmSquad_StockIconFrame',
             'warm_fighter', 'warm_copy', 'gmSquad_WarmFiles', 'due_member',
             'gmSquad_HoldRespawn', 'squad_retint', 'swap_in', 'gmSquad_OnVsFrame']
    budget_state = source[source.index('static u32 s_allm_budget;'):source.index('u32 mu_squad_main_heap')]
    budget_type = re.search(r'typedef struct SquadFileBudget \{.*?} SquadFileBudget;', source, re.S).group()
    defines += '\n' + '\n'.join(x for x in read(ROOT / 'port/runtime/abi/mu_squad_wire.h').splitlines() if x.startswith(('#define MU_SQUAD_ONLINE_', '#define MU_SQUAD_COMMAND ', '#define MU_SQUAD_OP_REPLAY_MATCH ', '#define MU_SQUAD_LINEUP ')))
    return defines + '\n' + member + '\n' + state + budget_state + budget_type + '\n' + '\n'.join(function(source, x) for x in names)


class SquadWiring(unittest.TestCase):
    def test_one_default_off_switch_removes_translation_units(self):
        cmake = read(GAME / 'CMakeLists.txt')
        self.assertIn('option(MELEE_SQUAD_STRIKE "Build Squad Strike for offline VS and Friends" OFF)', cmake)
        self.assertIn('target_compile_definitions(game_flags INTERFACE MU_SQUAD=1)', cmake)
        for name in ['mu_squad.c', 'mu_squad_css.c']:
            self.assertIn('list(REMOVE_ITEM SHIM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/shim/' + name + '")', cmake)
        for name in ['shim/mu_squad.c', 'shim/mu_squad_css.c', 'include/mu_squad.h']:
            source = read(GAME / name)
            self.assertIn('#if MU_SQUAD', source)
            self.assertIn(CREDIT, source)
            self.assertIn("Squad Strike idea: Chris's friend Tyler (Squad Strike Gecko codes).", source)
            self.assertNotIn('GPL', source)
            self.assertNotRegex(source, r'#include.*(?:pc/|grace|sdremix|ace/)')
        self.assertIn('licence not stated; used with the author\'s permission', read(ROOT / 'NOTICE'))

    @NEEDS_DECOMP
    def test_each_decomp_hook_compiles_out(self):
        expected = {
            'gm/gm_1601.c': 'gmSquad_HoldRespawn',
            'gm/gm_16A2.c': 'gmSquad_OnVsFrame',
            'gm/gmvs.c': 'gmSquad_WarmFiles',
            'gm/gmvsmelee.c': 'gmSquad_OnEnterVs',
            'if/ifstock.c': 'gmSquad_StockIconFrame',
            'mn/mncharsel.c': 'mnCharSel_SquadFrame',
            'lb/lbdvd.c': 'gmSquad_AddPreloads',
            'lb/lbheap.c': 'mu_squad_main_heap',
            'lb/lbaudio_ax.c': 'mu_squad_sound_heap',
            'pl/player.c': 'mu_squad_fighter_kinds',
            'lb/types.h': 'mu_squad_key',
            'ft/ftdata.c': 'mu_squad_fighter_files',
            'ft/kinds/ftKirby/ftkirby.c': 'mu_squad_copy_files',
        }
        patch = read(ROOT / 'sourceport/patches/melee-native.patch')
        for name, symbol in expected.items():
            source = read(DECOMP / name)
            self.assertIn(symbol, source, name)
            self.assertNotIn(symbol, without_squad(source), name)
            section = patch.split('diff --git a/src/melee/' + name + ' b/src/melee/' + name + '\n')[1].split('diff --git ')[0]
            self.assertIn('+' + '#if MU_SQUAD', section)
            self.assertIn(symbol, section)
            self.assertIn(CREDIT, section)
        self.assertNotIn('mu_squad', without_squad(read(DECOMP / 'mn/mncharsel.c')))

    def test_runtime_gate_is_stock_singles_vs_and_never_online_queue(self):
        source = read(GAME / 'shim/mu_squad.c')
        gate = function(source, 'gmSquad_ModeAllowed')
        self.assertIn('gm_GetCurrentGameMode() == GM_VS', gate)
        self.assertIn('mode == Mode_Stock', gate)
        self.assertNotIn('GM_HANYU_CSS ||', gate)
        enter = function(source, 'gmSquad_OnEnterVs')
        self.assertIn('!any || players != 2', enter)
        self.assertIn('start->rules.is_teams', enter)
        self.assertLess(enter.index('!any || players != 2'), enter.index('s_live = true'))
        self.assertLess(enter.index('s_live = true'), enter.index('start->players[i].stocks ='))

    @NEEDS_DECOMP
    def test_setup_parses_all_members_and_swap_has_no_loaders(self):
        source = read(GAME / 'shim/mu_squad.c')
        warm = function(source, 'gmSquad_WarmFiles')
        self.assertIn('i = 0; i < s_lineup_count[p]', warm)
        for call in ['warm_fighter(kind', 'warm_fighter(extra', 'warm_copy(kind', 'warm_copy(extra']:
            self.assertIn(call, warm)
        for call in ['ftData_8008572C', 'efAsync_LoadSync', 'ftData_80085820', 'ftData_80085A14']:
            self.assertIn(call, function(source, 'warm_fighter'))
        setup = function(read(DECOMP / 'gm/gmvs.c'), 'fn_8016E124')
        self.assertIn('gmSquad_WarmFiles();', setup)
        for name in ['swap_in', 'gmSquad_OnVsFrame']:
            swap = function(source, name)
            self.assertNotRegex(swap, r'WarmFiles|warm_fighter|warm_copy|fn_8016E124|ftData_|lbDvd_|lbArchive_|80031DA8')
        self.assertIn('s_held_frame[slot] == gmSquad_Frame()', function(source, 'gmSquad_OnVsFrame'))
        self.assertIn('s_current[slot]', function(source, 'due_member'))

    def test_hash_is_identical_unless_squad_live(self):
        source = read(GAME / 'shim/mu_online.c')
        # Preserve the complete original expression/order; only a live squad adds kind.
        current = function(source, 'compute_checksum')
        self.assertIn('if (gmSquad_Live()) acc ^= (u32) fp->kind;', current)
        # Frozen from yg-build-50m's function, including expression order. Avoid
        # a mutable HEAD baseline and lazy network fetches in partial clones.
        self.assertEqual(hashlib.sha256(without_squad(current).encode()).hexdigest(),
                         '2ce73b7096b1c8be9da92b169d297d493ce9f8ad3bed9ded8a6f4d134ccdafb0')
        self.assertNotIn('gmSquad', without_squad(source))

    def test_join_uses_checked_capture_and_apply_without_abi_growth(self):
        source = read(GAME / 'include/mu_css_transfer.inc')
        capture = source[source.index('if (op == MU_CSS_CAPTURE ||'):source.index('if (op != MU_CSS_APPLY')]
        self.assertNotIn('mnCharSel_SquadJoin()', capture)
        self.assertEqual(source.count('mnCharSel_SquadJoin();'), 1)
        self.assertIn('if (saved_stocks >= 0) s->rules[4] = (u8) saved_stocks;', capture)
        self.assertLess(source.index('op != MU_CSS_APPLY || !mu_css_valid(s)'),
                        source.index('mnCharSel_SquadJoin();'))
        self.assertLess(source.index('if (tags[i] == 120) return -1;'),
                        source.index('mnCharSel_SquadJoin();'))
        self.assertLess(source.index('mnCharSel_SquadJoin();'), source.index('#define MU_CSS_WRITE_RULE'))
        self.assertNotIn('Squad', without_squad(source))
        abi = read(ROOT / 'port/runtime/abi/mu_css_join.h')
        self.assertNotIn('squad', abi.lower())
        join = function(read(GAME / 'shim/mu_squad_css.c'), 'mnCharSel_SquadJoin')
        self.assertIn('gmSquad_ClearAll()', join)
        for array, value in [('squad_seen_icon', '0xFF'), ('squad_icons_shown', '0'), ('squad_shown', '0')]:
            self.assertIn(f'memset({array}, {value}, sizeof({array}))', join)
        self.assertIn('squad: cleared on join\\n', join)

    @NEEDS_DECOMP
    def test_synced_randomness_and_original_ui_controls(self):
        source = read(GAME / 'shim/mu_squad.c')
        random = function(source, 'gmSquad_Random')
        self.assertIn('HSD_Randi(0x7FFF)', random)
        self.assertNotIn('gmSquad_Frame()', random)
        vanilla = function(read(DECOMP / 'mn/mncharsel.c'), 'mnCharSel_8025FB50')
        self.assertIn('HSD_Randi(CSS_NONE)', vanilla)
        host = read(ROOT / 'port/app/source_host.cpp')
        self.assertIn('g_css_origin.frame < q->frame', host)
        self.assertIn('frame_hash(native_savestate::hash_spans(g_css_origin.spans))', host)
        self.assertIn('mu_gm_engine_state()->unk_0', function(source, 'gmSquad_Frame'))
        css = read(GAME / 'shim/mu_squad_css.c')
        self.assertNotRegex(css + source, r'\b(?:OSGetTick|OSGetTime|time|gettimeofday|VIGetRetraceCount)\(')
        for key in ['HSD_PAD_Z', 'HSD_PAD_R', 'HSD_PAD_L', 'HSD_PAD_X', 'HSD_PAD_Y', 'SQUAD_CLEAR_HOLD 90']:
            self.assertIn(key, css)
        self.assertIn('HSD_PAD_DPADUP) && !mu_console_active()', css)
        self.assertIn('HSD_SisLib_803A6754', css)
        buttons = function(css, 'squad_buttons')
        self.assertEqual(buttons.count('trigger &= ~HSD_PAD_Z'), 2)
        self.assertLess(buttons.index('if (!(pressed & HSD_PAD_Z)) return;'),
                        buttons.index('trigger &= ~HSD_PAD_Z'))
        self.assertLess(buttons.index('if (squad_on_random(cursor))'),
                        buttons.index('trigger &= ~HSD_PAD_Z'))
        portrait = buttons[buttons.index('int ck = squad_hovered(cursor);'):]
        self.assertLess(portrait.index('if (ck >= 0 && ck < 26)'), portrait.index('trigger &= ~HSD_PAD_Z'))
        self.assertIn('trigger &= ~HSD_PAD_L', buttons)
        self.assertLess(buttons.index('trigger &= ~consumed'), buttons.index('squad_remove_last(target)'))

    @NEEDS_DECOMP
    def test_budgets_and_stock_rules_are_bounded(self):
        source = read(GAME / 'shim/mu_squad.c')
        budget = function(source, 'gmSquad_Budget')
        for call in ['mu_squad_fighter_files', 'mu_squad_copy_files', 's_allm_budget', 's_alla_budget']:
            self.assertIn(call, budget)
        file = function(source, 'squad_budget_file')
        self.assertIn('gmSquad_FileSize', file)
        cached = function(source, 'gmSquad_FileSize')
        for word in ['DVDConvertPathToEntrynum', 'DVDFastOpen', 'info.length', 's_disc_sizes']:
            self.assertIn(word, cached)
        self.assertNotRegex(budget + file, r'CanFit|native_alloc|mu_host|free_space')
        enter = function(source, 'gmSquad_OnEnterVs')
        self.assertLess(enter.index('squad_trim(start)'), enter.index('s_live = true'))
        self.assertIn('GMSQUAD_EXTRA - 1', function(source, 'squad_trim'))
        self.assertIn('squad: trimmed P%d', function(source, 'squad_trim'))
        css = read(GAME / 'shim/mu_squad_css.c')
        # Every pick goes through squad_fits: the budget check plus the door's refusal note.
        self.assertIn('squad_fits(target, -1, GMSQUAD_NONE, 0)', function(css, 'squad_add'))
        fits = function(css, 'squad_fits')
        self.assertIn('if (squad_budget(coin_port, coin_ck, coin_color)) return true;', fits)
        self.assertIn('squad_note[target] = SQUAD_NOTE_FRAMES;', fits)
        self.assertIn('squad_fits(p, -1, GMSQUAD_NONE, 0)', function(css, 'squad_fill_on_drop'))
        self.assertIn('Squads too big to load together: pick a lighter fighter.', function(css, 'squad_line'))
        random = function(css, 'squad_random_pick')
        self.assertIn('squad_place_coin(target, cursor, ck, color)', random)
        place = function(css, 'squad_place_coin')
        self.assertLess(place.index('!squad_fits(target, target, ck, color)'), place.index('coin_model->x5 = 0'))
        self.assertLess(place.index('squad_clash(target, ck, color)'), place.index('coin_model->x5 = 0'))
        self.assertIn('squad_restore_stocks();', function(css, 'mnCharSel_SquadExit'))
        self.assertIn('gmSquad_StockRule(active', function(css, 'squad_sync_stocks'))
        heap = read(DECOMP / 'lb/lbheap.c')
        self.assertEqual(heap.count('squad: heaps allM=%u allA=%u aram1=%u main=%u'), 1)
        self.assertIn('arena_hi > arena_lo', heap)

    @NEEDS_DECOMP
    def test_fighter_destructor_clears_item_references(self):
        unload = function(read(DECOMP / 'ft/fighter.c'), 'Fighter_Unload_8006DABC')
        self.assertLess(unload.index('it_8026B7F8(fp->gobj)'), unload.index('HSD_ObjFree(&fighter_alloc_data'))
        items = read(DECOMP / 'it/it_26B1.c')
        self.assertIn('evt_unk, cur, fighter_gobj', function(items, 'it_8026B7F8'))
        self.assertIn('ip->owner = NULL', function(items, 'it_8026B894'))
        for name in ['itlinkbomb', 'itsamusmissile', 'itpeachturnip', 'itgamewatchchef', 'itkirbygamewatchchefpan']:
            self.assertIn('it_8026B894(', read(DECOMP / ('it/kinds/' + name + '.c')))

    def test_four_fighter_pick_undo_clear_random_and_recolour(self):
        """The online row's pick through the production CSS functions (squad_pick4.c)."""
        cc = shutil.which('cc')
        if not cc:
            self.skipTest('C compiler required')
        css = read(GAME / 'shim/mu_squad_css.c')
        engine = read(GAME / 'shim/mu_squad.c')
        header = read(GAME / 'include/mu_squad.h')
        types = '\n'.join(x for x in header.splitlines() if x.startswith('#define GMSQUAD_'))
        types += '\n' + re.search(r'typedef struct GmSquadMember \{.*?} GmSquadMember;', header, re.S).group() + '\n'
        types += re.search(r'#define SQUAD_NOTE_FRAMES \d+', css).group() + '\n'
        names = ['squad_fill_mode', 'squad_coin_held', 'squad_coin_pick', 'squad_players', 'squad_clash', 'squad_fix_color',
                 'squad_fits', 'squad_cycle_color', 'squad_remove_last', 'squad_add', 'squad_add_member',
                 'squad_free_hand', 'squad_coin_key', 'squad_fill_on_drop', 'squad_hovered',
                 'squad_on_random', 'squad_icon_of', 'squad_place_coin', 'squad_rival_at', 'squad_random_pick', 'squad_xy',
                 'squad_b_track', 'squad_fill_buttons']
        fixture = function(engine, 'gmSquad_Count') + function(engine, 'gmSquad_Clear') + function(engine, 'gmSquad_Lineup')
        fixture += ''.join(function(css, name) for name in names)
        with tempfile.TemporaryDirectory(prefix='squad-pick4-', dir=os.environ.get('TMPDIR')) as directory:
            temp = Path(directory)
            (temp / 'mu_squad_pick4_types.h').write_text(types)
            (temp / 'mu_squad_pick4_fixture.h').write_text(fixture)
            build = subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                                    '-DMU_SQUAD=1', '-fsanitize=address,undefined', '-I', directory,
                                    '-I', str(ROOT / 'port/runtime/abi'), str(GAME / 'tests/squad_pick4.c'),
                                    '-o', str(temp / 'pick4')], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(temp / 'pick4')], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('pick, undo, clear, random without duplicates, recolour, no Z: PASS', run.stdout)
            self.assertIn('a refused fourth pick comes back with a note: PASS', run.stdout)
            self.assertIn('a coin no hand dropped (door opening, random) gets the budget check: PASS', run.stdout)
            self.assertIn('A on a fighter already picked buzzes, undo unmarks it: PASS', run.stdout)
            build = subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                                    '-DMU_SQUAD=1', '-DFIXTURE_LOBBY=1', '-fsanitize=address,undefined', '-I', directory,
                                    '-I', str(ROOT / 'port/runtime/abi'), str(GAME / 'tests/squad_pick4.c'),
                                    '-o', str(temp / 'lobby-pick4')], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(temp / 'lobby-pick4')], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('pick, undo, clear, random without duplicates, recolour, no Z: PASS', run.stdout)

    def test_production_c_lineup_stocks_warm_and_swap_behavior(self):
        cc = shutil.which('cc')
        if not cc:
            self.skipTest('C compiler unavailable')
        fixture = production_fixture()
        with tempfile.TemporaryDirectory(prefix='squad-c-test-', dir=os.environ.get('TMPDIR')) as directory:
            temp = Path(directory)
            (temp / 'mu_squad_lineup_fixture.h').write_text(fixture)
            css = read(GAME / 'shim/mu_squad_css.c')
            (temp / 'mu_squad_css_fixture.h').write_text(
                function(css, 'squad_xy') + function(css, 'squad_b_track') + function(css, 'squad_fill_buttons') +
                function(css, 'squad_buttons') + function(css, 'mnCharSel_SquadJoin'))
            build = subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Wno-unused-function',
                                    '-I', directory, '-I', str(ROOT / 'port/runtime/abi'), str(GAME / 'tests/squad_lineup.c'), '-o', str(temp / 'test')],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(temp / 'test')], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('PASS', run.stdout)


    def test_program_and_library_refuse_a_mixed_squad_install(self):
        lib = read(ROOT / 'sourceport/game/shim/mu_squad.c')
        self.assertIn('MU_EXPORT int mu_squad_build(void) { return MU_SQUAD_VERSION; }', lib)
        self.assertLess(lib.index('mu_squad_build'), lib.rindex('#endif'))
        host = read(ROOT / 'port/app/source_host.cpp')
        at = host.index('game_proc("mu_squad_build")')
        block = host[at:host.index('set_native_bridge(console_practice_bridge)', at)]
        self.assertIn('#if MU_SQUAD\n    if (!squad_build || squad_build() != MU_SQUAD_VERSION)\n#else\n    if (squad_build)\n#endif', block)
        self.assertIn('host::die(', block)
        self.assertLess(at, host.index('slippi::native_practice::set_native_bridge(console_practice_bridge)'))
        self.assertGreater(at, host.index('if (entry(&api, &g_game) != 0)'))

if __name__ == '__main__':
    unittest.main()
