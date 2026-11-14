/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026).
 * Squad Strike idea: Chris's friend Tyler (Squad Strike Gecko codes).
 */
/* Behavioral fixture: the generated header contains the production squad
 * functions unchanged. Only game services and layouts are stubbed here. */
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <mu_lobby_wire.h>
static int mu_lobby_css(void) { return 0; }
static int mu_lobby_send(const unsigned char* p, unsigned n) { (void)p; (void)n; return 1; }
bool gmSquad_LobbyChosen(void);
#if !MU_SQUAD_ONLINE_FIXTURE
bool gmSquad_LobbyChosen(void) { return false; }
#endif

typedef uint8_t u8;
typedef int8_t s8;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef float f32;
typedef int CharacterKind;
enum { GM_HANYU_CSS = 8, GM_DEBUG_VS = 14, GM_VS = 2, GM_MAX_PLAYERS = 6, ChKind_Max = 33, ChKind_None = 33,
       CKind_Kirby = 4, Ft_Kind_Kirby = 4, CKind_PopoNana = 14,
       Gm_PKind_Human = 0, Gm_PKind_Cpu = 1, Gm_PKind_NA = 3,
       MatchKind_Stock = 1 };
typedef struct HSD_GObj { int value; } HSD_GObj;
typedef struct {
    int ckind, color, slot_type, stocks, sub_color;
} PlayerInitData;
typedef struct {
    struct { int match_kind, is_teams, stkind; } rules;
    PlayerInitData players[GM_MAX_PLAYERS];
} StartMeleeData;
typedef struct {
    int pkind, player_state, stocks, ckind, color;
    HSD_GObj* player_entity[2];
    u8 transformed[2];
    struct { bool b2; } flags;
} StaticPlayer;
struct GameCache { struct { int char_id; u8 color; } entries[8]; u32 mu_squad_key; };

/* The real game uses mu_entry.c's host command wrapper in both build variants. */
static int fixture_replay_squad;
static int fixture_harness;
int mu_online_harness(void) { return fixture_harness; }
/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026) */
static int fixture_launch_query;
int mu_online_launch_query(void) { return fixture_launch_query; }
int mu_slippi_command(u8 command, const u8* payload, unsigned int size,
                      u8* response, unsigned int capacity, unsigned int* got)
{
    assert(command == 0xEC && size == 2 && payload[0] == 8 && payload[1] <= 1);
    assert(!response && !capacity && !got);
    fixture_replay_squad = payload[1];
    return 0;
}

static StaticPlayer players[4];
static HSD_GObj entities[4][2];
static int allowed = 1, major = GM_VS, frees, creates, respawns, nana_sleeps;
static int warms, copy_warms, preload_fighters, preload_copies;
static u32 frame;
static u8 ftData_UnkBytePerCharacter[40];
static StaticPlayer* Player_GetPtrForSlot(int p) { return &players[p]; }
static int Player_GetPlayerSlotType(int p) { return players[p].pkind; }
static int gm_GetCurrentGameMode(void) { return major; }
static bool fixture_online_chosen, fixture_online_ready;
static bool gmSquad_OnlineChosen(void) { return fixture_online_chosen; }
static bool gmSquad_OnlineReady(void) { return fixture_online_ready; }
static bool fixture_editable;
static bool gmSquad_OnlineEditable(void) { return fixture_editable; }
static bool gmSquad_ModeAllowed(void) { return allowed; }
static u32 gmSquad_Frame(void) { return frame; }
static s32 mu_squad_fighter_kinds(CharacterKind ck, s32* extra)
{ *extra = ck == 14 ? 15 : ck == 18 ? 19 : -1; return ck; }
static void ftData_8008572C(int k) { (void)k; ++warms; }
static void efAsync_LoadSync(int k) { (void)k; ++warms; }
static void ftData_80085820(int k, int c) { (void)k; (void)c; ++warms; }
static void ftData_80085A14(int k) { (void)k; ++warms; }
static void Player_80031DA8(int k, int c) { (void)k; (void)c; ++copy_warms; }
static void Player_80031CB0(int k, int c) { (void)k; (void)c; ++preload_fighters; }
static void Player_80031D2C(int k, int c) { (void)k; (void)c; ++preload_copies; }
static void Player_80031EBC(int p) { (void)p; ++frees; }
static void Player_SetPlayerCharacter(int p, int k) { players[p].ckind = k; }
static void Player_SetCostumeId(int p, int c) { players[p].color = c; }
static int Player_GetPlayerCharacter(int p) { return players[p].ckind; }
static int Player_GetCostumeId(int p) { return players[p].color; }
static s8 fixture_sub_color[4];
static void Player_SetSubColor(int p, s8 c) { fixture_sub_color[p] = c; }
typedef struct { u8 r, g, b, a; } FixtureColor;
#define GXColor FixtureColor
typedef struct { u8 sub_color; GXColor x610_color_rgba[2]; } Fighter;
static struct { GXColor sub_colors[4]; } fixture_common = {{{255, 0, 0, 51}, {0, 255, 0, 102}, {0, 0, 255, 153}, {255, 255, 255, 204}}};
#define p_ftCommonData (&fixture_common)
static Fighter fixture_fighters[4][2];
#define GET_FIGHTER(g) (&fixture_fighters[((g) - &entities[0][0]) / 2][((g) - &entities[0][0]) % 2])
static void Player_80031AD0(int p)
{ players[p].player_entity[0] = &entities[p][0];
  players[p].player_entity[1] = &entities[p][1]; ++creates; }
static void Player_80031848(int p) { (void)p; }
static void ftCo_800D4F24(HSD_GObj* g, int x) { (void)g; (void)x; ++nana_sleeps; }
static f32 gm_80168B34(int k, int ignored, int c) { (void)ignored; return k * 10.f + c; }
static bool gmSquad_HoldRespawn(s32, s32);
static void fn_8016719C(int p, int sub)
{ ++respawns; assert(!gmSquad_HoldRespawn(p, sub)); }
/* Log lines the production code writes, kept so tests can check the ones they care about. */
static char fixture_logs[8192];
static void fixture_log(const char* format, ...)
{
    size_t n = strlen(fixture_logs);
    va_list ap;
    if (n + 1 >= sizeof fixture_logs) return;
    va_start(ap, format);
    vsnprintf(fixture_logs + n, sizeof fixture_logs - n, format, ap);
    va_end(ap);
}
#define OSReport fixture_log

/* Disc table fixture: duplicate file IDs share one archive, as the real
 * DVD entry-number API does. No allocator/free-space state affects lengths. */
typedef struct { u32 length; } DVDFileInfo;
static unsigned file_opens;
/* 0..127 game files; with fixture_costumes, 200 + kind * 6 + colour are the fighter costumes
 * and 440 + kind * 6 + colour Kirby's colour hats (the files a skin replaces). */
static u32 file_bytes[640];
static char file_names[640][16];
static int fixture_costumes;
static char* lbFileGetFullName(const char* name) { return (char*) name; }
static int fixture_missing = -1; /* a file the disc lacks */
static int DVDConvertPathToEntrynum(const char* name) { return atoi(name) == fixture_missing ? -1 : atoi(name); }
static int DVDFastOpen(int file, DVDFileInfo* info)
{ ++file_opens; if (file < 0 || file >= 640) return 0; info->length = file_bytes[file]; return 1; }
static void DVDClose(DVDFileInfo* info) { (void)info; }
static const char* mu_ground_stage_file(int stage) { (void)stage; return "100"; }
static int Stage_8022519C(int stage) { return stage; }
static int gm_801641CC(u8 index) { return index; }
typedef void (*GmSquadFileVisitor)(void*, const char*, int);
u32 gmSquad_CostumeBytes(int kind, int use);
/* Visit uses 2 and 3 are GMSQUAD_FILE_COSTUME and GMSQUAD_FILE_HAT (asserted below). */
static void mu_squad_fighter_files(int kind, u8 color, void* ctx, GmSquadFileVisitor visit)
{ if (kind < 0) return; visit(ctx, file_names[kind], true);
  visit(ctx, file_names[kind + 40], false);
  if (fixture_costumes) visit(ctx, file_names[200 + kind * 6 + color], 2); }
static void mu_squad_copy_files(int kind, u8 color, void* ctx, GmSquadFileVisitor visit)
{ if (kind >= 0 && kind != 4) visit(ctx, file_names[kind + 80], false);
  if (fixture_costumes && kind >= 0 && kind != 4 && gmSquad_CostumeBytes(kind, 3))
      visit(ctx, file_names[440 + kind * 6 + color], 3); }
/* Friends' synced select (a console session): the fixed online floors decide, not this host. */
static int fixture_console;
static int mu_console_active(void) { return fixture_console; }
/* Friends' costume rule: the real one (mu_squad_online.c) in the online fixture, else a stand-in. */
u8 gmSquad_ConsoleCostumes(bool want);
#if !MU_SQUAD_ONLINE_FIXTURE
u8 gmSquad_ConsoleCostumes(bool want) { return want ? 1 : 0; }
#endif
/* The test-only Kirby copy check reads the game's Kirby tables; the fixture has none. */
static void test_kirby_copies(void) {}
#include "mu_squad_lineup_fixture.h"
_Static_assert(GMSQUAD_FILE_COSTUME == 2 && GMSQUAD_FILE_HAT == 3, "fixture visit uses");

/* Run the production button handler and join reset against small UI services. */
enum { HSD_PAD_Z=1, HSD_PAD_R=2, HSD_PAD_L=4, HSD_PAD_X=8,
       HSD_PAD_Y=16, HSD_PAD_DPADUP=32, HSD_PAD_START=64, HSD_PAD_A=128, HSD_PAD_B=256,
       SQUAD_CLEAR_HOLD=90 };
struct CSSCursorData { int x5, x6; };
static int mnCharSel_804D6CF0;
static struct CSSCursorData cursors[4];
static struct CSSCursorData* mnCharSel_804A0BC0[4] = {&cursors[0],&cursors[1],&cursors[2],&cursors[3]};
static struct { u32 trigger, button; } HSD_PadCopyStatus[4];
static u8 squad_l_held[4], squad_seen_icon[4], squad_b_held[4];
static bool squad_b_owned[4];
static bool squad_b_down[4];
static bool squad_b_edge[4];
static u32 squad_icons_shown[4], squad_shown[4], squad_marks_shown;
static u8 squad_note[4];
static bool squad_icons_tried;
static int hover=-1, random_area, ui_players=2, ui_adds, ui_randoms, buzzes;
static int member_under = -1, recolors, recolor_dir, recolor_index = -1;
static bool fixture_recolor_ok = true;
static int squad_member_under_cursor(int p, int* owner) { *owner = p; return member_under; }
static bool squad_cycle_color(int p,int i,int dir)
{ (void)p; recolor_index = i; recolor_dir = dir; ++recolors; return fixture_recolor_ok; }
static bool squad_door_open(int p) { return p>=0 && p<4; }
static int moves;
static void sfxMove(void) { ++moves; }
static int backs;
static void sfxBack(void) { ++backs; }
static void lbAudioAx_80024030(int sound) { assert(sound==3);++buzzes; }
static bool squad_remove_last(int p) { if (!gmSquad_Count(p)) return false; gmSquad_Clear(p); return true; }
static bool squad_on_random(struct CSSCursorData* cursor) { (void)cursor;return random_area; }
static int squad_players(void) { return ui_players; }
static int squad_hovered(struct CSSCursorData* cursor) { (void)cursor;return hover; }
static void squad_random_pick(int p,struct CSSCursorData* cursor) { (void)p;(void)cursor;++ui_randoms; }
static void squad_add(int p,u8 ck) { (void)p;(void)ck;++ui_adds;if(ui_players>2)lbAudioAx_80024030(3); }
static void squad_sync_stocks(void) {}
static void squad_banners(void) {}
/* The online row's fill mode (three slots filled by coin drops). */
static bool fixture_fill_mode;
static u8 fixture_coin_pick = GMSQUAD_NONE;
static int regrabs, regrab_door = -1, regrab_cport = -1;
static bool squad_fill_mode(void) { return fixture_fill_mode; }
static u8 squad_coin_pick(int p) { (void)p; return fixture_coin_pick; }
static void squad_regrab(int door, int cport) { ++regrabs; regrab_door = door; regrab_cport = cport; }
static bool fixture_coin_in_hand = true;
static bool squad_coin_held(int p) { (void)p; return fixture_coin_in_hand; }
static struct { struct GameCache game_cache; } preload_scene;
#define lbDvd_GetPreloadCacheScene() (&preload_scene)
#include "mu_squad_css_fixture.h"

#if MU_SQUAD_ONLINE_FIXTURE
#include <mu_squad_wire.h>
#include <mu_host_aram.h>
static MuSquadSetup fixture_reply;
static MuSquadSide fixture_host_side;
static bool fixture_reply_valid = true;
/* The host's squadLineup answer (MU_SQUAD_OP_AGENT_LINEUP): fixture_agent_size bytes, 0 = none. */
static u8 fixture_agent[2 * MU_SQUAD_LINEUP];
static unsigned fixture_agent_size, agent_queries;
static bool fixture_host_support = true, fixture_accept_selection = true;
static unsigned facts_sent, commands_sent;
static u8 fixture_budget_answer = 1; /* the host's op 4 answer: 1 ok, 0 none, 2 disc copy over, 3 mod's */
/* FNV-1a over every budget digest byte (ops 2 and 3), as the host's SHA-256 identity sees them. */
static u64 fixture_identity;
/* Friends' costume rule (MU_SQUAD_OP_CONSOLE): facts held since the last off, and the ARM answer
 * once the host has them. */
static unsigned console_facts, console_arms, console_offs;
static bool console_have_facts;
static u8 fixture_console_answer = 1;
enum { EF_DAT_FILE_MAX = 3 };
static void mu_squad_effect_file(int index, void* ctx, GmSquadFileVisitor visit)
{ visit(ctx, file_names[index+70], false); }
static int mu_slippi_cmd(unsigned cmd, const void* payload, unsigned size,
                         void* out, unsigned capacity, unsigned* got)
{
    const u8* p = payload;
    ++commands_sent;
    assert(cmd == MU_SQUAD_COMMAND && size > 0);
    if (got) *got = 0;
    if (!fixture_host_support) return 0; // old host silently ignores 0xEC
    if (p[0] == 5) {
        *got = fixture_reply_valid ? (unsigned) mu_squad_encode(&fixture_reply, out, capacity) : 0;
    } else if (p[0] == MU_SQUAD_OP_AGENT_LINEUP) {
        assert(size == 1 && capacity >= sizeof fixture_agent);
        ++agent_queries;
        memcpy(out, fixture_agent, fixture_agent_size);
        *got = fixture_agent_size;
    } else if (p[0] == 3) {
        ++facts_sent;
        assert(size >= 10 && size <= 137);
        for (unsigned i = 1; i < size; ++i) fixture_identity = (fixture_identity ^ p[i]) * 1099511628211ull;
    } else if (p[0] == 2) {
        assert(size == 9);
        fixture_identity = 14695981039346656037ull;
        assert(((u32)p[1]<<24 | (u32)p[2]<<16 | (u32)p[3]<<8 | p[4]) == MU_SQUAD_ONLINE_ALLM);
        assert(((u32)p[5]<<24 | (u32)p[6]<<16 | (u32)p[7]<<8 | p[8]) == MU_SQUAD_ONLINE_ALLA);
    } else if (p[0] == MU_SQUAD_OP_CONSOLE) {
        if (p[1] == MU_SQUAD_CONSOLE_FACT) {
            const u32 kind = (u32)p[3]<<24 | (u32)p[4]<<16 | (u32)p[5]<<8 | p[6];
            const u32 bytes = (u32)p[7]<<24 | (u32)p[8]<<16 | (u32)p[9]<<8 | p[10];
            /* op 3's layout after the console op: a cosmetic at its fighter's constant, delimited */
            assert(size >= 13 && size <= 138 && (p[2] == 2 || p[2] == 3));
            assert(p[size - 1] == 0 && strlen((const char*) p + 11) == size - 12);
            assert(bytes == gmSquad_CostumeBytes((int) kind, p[2]) && bytes != 0);
            ++console_facts; console_have_facts = true;
        } else if (p[1] == MU_SQUAD_CONSOLE_ARM) {
            assert(size == 2 && got && capacity >= 1);
            ++console_arms;
            *(u8*)out = console_have_facts ? fixture_console_answer : 0; *got = 1;
        } else {
            assert(size == 2 && p[1] == MU_SQUAD_CONSOLE_OFF);
            ++console_offs; console_have_facts = false;
        }
    } else if (p[0] == 0 || p[0] == 1 || p[0] == 4) {
        bool lineup_ok = true;
        if (p[0] == 1) {
            /* Same bound and rule as the host's squad_command: never past members[],
             * and only exactly MU_SQUAD_LINEUP valid fighters are accepted. */
            assert(p[1] <= MU_SQUAD_LINEUP && size == 2u + 2u * p[1]);
            memset(&fixture_host_side, 0, sizeof fixture_host_side);
            fixture_host_side.count = p[1];
            for (unsigned i = 0; i < p[1]; ++i)
                fixture_host_side.members[i] = (MuSquadFighter){p[2 + 2*i], p[3 + 2*i]};
            lineup_ok = mu_squad_side_valid(&fixture_host_side);
            if (!lineup_ok) memset(&fixture_host_side, 0, sizeof fixture_host_side);
        }
        if (got && capacity && (p[0] != 1 || (fixture_accept_selection && lineup_ok))) {
            *(u8*)out = p[0] == 4 ? fixture_budget_answer : 1; *got = 1;
        }
    } else assert(0);
    return 0;
}
#define online_chosen fixture_online_chosen
#define online_ready fixture_online_ready
#include "mu_squad_online_fixture.h"
#undef online_chosen
#undef online_ready
/* The production squadLineup CSS steps and "squad: picks" line, against stand-in pickers:
 * members join gmSquad_rows[0], the coin sets the coin pick, and kind 25 is locked. */
struct CSSCharModel { int unused; };
static struct CSSCharModel coin_models[4];
static struct CSSCharModel* mnCharSel_804A0BD0[4] = {&coin_models[0],&coin_models[1],&coin_models[2],&coin_models[3]};
static u8 mnCharSel_804D6CF2, mnCharSel_804D6CF6, mnCharSel_804D6CF7;
static u8 agent_coin_ck = GMSQUAD_NONE, agent_coin_color;
#define agent_coin_held fixture_coin_in_hand /* one coin-in-hand switch for both stand-ins */
static bool agent_refuse_coin;
static int agent_adds, agent_coins;
static int squad_icon_of(u8 ck) { return ck < 25 ? ck : -1; }
static bool squad_add_member(int p, u8 ck, u8 color)
{
    int i;
    for (i = 0; i < GMSQUAD_EXTRA && gmSquad_rows[p][i].ckind != GMSQUAD_NONE; ++i) {}
    if (i == GMSQUAD_EXTRA) return false;
    gmSquad_rows[p][i] = (GmSquadMember){ck, color};
    ++agent_adds;
    return true;
}
static bool squad_place_coin(int p, struct CSSCursorData* cursor, u8 ck, u8 color)
{
    (void)p;
    if (agent_refuse_coin) return false;
    agent_coin_ck = ck; agent_coin_color = color; agent_coin_held = false;
    cursor->x5 = 2; ++agent_coins;
    return true;
}
static int gmSquad_CssPreview(u8* coin_ck, u8* coin_color, GmSquadMember out[GMSQUAD_SIZE])
{
    *coin_ck = agent_coin_ck; *coin_color = agent_coin_color;
    return gmSquad_Lineup(0, *coin_ck, *coin_color, out);
}
#include "mu_squad_agent_css_fixture.h"
static void fixture_own_extras(void)
{
    gmSquad_rows[0][0] = (GmSquadMember){14,0}; /* coin pick, then ICs, Zelda and kind 9 */
    gmSquad_rows[0][1] = (GmSquadMember){18,0};
    gmSquad_rows[0][2] = (GmSquadMember){9,0};
    gmSquad_coin_pos[0] = 0;
}
static void online_admission_test(void)
{
    unsigned opens, facts, calls, i;
    clock_t begin, scanned, cached;
    fixture_online_chosen = false;
    fixture_host_support = false;
    gmSquad_OnlineChoose(true);
    assert(!gmSquad_OnlineBudget() && !gmSquad_OnlineSearchReady());
    assert(strstr(gmSquad_OnlineError(), "Host has no"));
    assert(!gmSquad_OnlineSelect(2,0) && !gmSquad_OnlineSearchReady());
    gmSquad_OnlineChoose(false); fixture_host_support = true;
    gmSquad_OnlineChoose(true);
    s_allm_budget = MU_SQUAD_ONLINE_ALLM - 1; s_alla_budget = MU_SQUAD_ONLINE_ALLA;
    assert(!gmSquad_OnlineBudget() && strstr(gmSquad_OnlineError(), "platform"));
    s_allm_budget = MU_SQUAD_ONLINE_ALLM;
    s_alla_budget = MU_SQUAD_ONLINE_ALLA - 1;
    assert(!gmSquad_OnlineBudget() && strstr(gmSquad_OnlineError(), "platform"));
    s_alla_budget = MU_SQUAD_ONLINE_ALLA;
    s_disc_count = 0; budget_scanned = false; file_opens = 0; facts_sent = 0;
    begin = clock();
    assert(gmSquad_OnlineBudget());
    scanned = clock() - begin;
    opens = file_opens; facts = facts_sent; calls = commands_sent;
    assert(opens > 0 && facts > 100);
    /* Exactly four fighters: one to three are refused with the count, and the host's
     * old lineup is cleared with a bounded count-0 send. */
    fixture_host_side = (MuSquadSide){4, {{2,0},{14,0},{18,0},{9,0}}};
    assert(!gmSquad_OnlineSelect(2,0) && !gmSquad_OnlineSearchReady());
    assert(!strcmp(gmSquad_OnlineError(), "Pick 4 fighters first (1/4)."));
    assert(fixture_host_side.count == 0);
    gmSquad_rows[0][0] = (GmSquadMember){14,0}; gmSquad_coin_pos[0] = 0;
    fixture_host_side = (MuSquadSide){4, {{2,0},{14,0},{18,0},{9,0}}};
    assert(!gmSquad_OnlineSelect(2,0) && !gmSquad_OnlineSearchReady());
    assert(!strcmp(gmSquad_OnlineError(), "Pick 4 fighters first (2/4)."));
    assert(fixture_host_side.count == 0);
    gmSquad_rows[0][1] = (GmSquadMember){18,0};
    fixture_host_side = (MuSquadSide){4, {{2,0},{14,0},{18,0},{9,0}}};
    assert(!gmSquad_OnlineSelect(2,0) && !gmSquad_OnlineSearchReady());
    assert(!strcmp(gmSquad_OnlineError(), "Pick 4 fighters first (3/4)."));
    assert(fixture_host_side.count == 0);
    gmSquad_rows[0][1] = (GmSquadMember){GMSQUAD_NONE,0};
    assert(!gmSquad_OnlineSelect(GMSQUAD_NONE,0) && !gmSquad_OnlineSearchReady());
    assert(!strcmp(gmSquad_OnlineError(), "Pick 4 fighters first (1/4)."));
    /* START short of four never reaches lock-in (the coin is in the hand): the CSS says the same
     * line, which stays while the count does and goes once it changes. Other errors stay. */
    gmSquad_OnlineNeedLineup(3);
    assert(!strcmp(gmSquad_OnlineError(), "Pick 4 fighters first (3/4)."));
    gmSquad_OnlinePickCount(3);
    assert(!strcmp(gmSquad_OnlineError(), "Pick 4 fighters first (3/4)."));
    gmSquad_OnlinePickCount(4);
    assert(!gmSquad_OnlineError()[0]);
    gmSquad_OnlineNeedLineup(-1);
    assert(!strcmp(gmSquad_OnlineError(), "Pick 4 fighters first (0/4)."));
    gmSquad_OnlinePickCount(1);
    assert(!gmSquad_OnlineError()[0]);
    fixture_own_extras();
    fixture_logs[0] = 0;
    assert(gmSquad_OnlineSelect(2,0) && gmSquad_OnlineSearchReady());
    assert(!strcmp(fixture_logs, "squad: locked in 4/4 2:0,14:0,18:0,9:0\n"));
    assert(fixture_host_side.count == 4 && fixture_host_side.members[0].kind == 2);
    assert(fixture_host_side.members[1].kind == 14 && fixture_host_side.members[2].kind == 18);
    assert(fixture_host_side.members[3].kind == 9);
    /* The coin's colour past its fighter's last (an m-ex 7th) locks in as its last colour. */
    assert(gmSquad_OnlineSelect(2, 9) && gmSquad_OnlineSearchReady());
    assert(fixture_host_side.members[0].kind == 2 && fixture_host_side.members[0].color == mu_squad_colors(2) - 1);
    assert(mu_squad_side_valid(&fixture_host_side));
    fixture_accept_selection = false;
    fixture_logs[0] = 0;
    assert(!gmSquad_OnlineSelect(3,1) && !gmSquad_OnlineSearchReady());
    assert(!strstr(fixture_logs, "locked in"));
    assert(strstr(gmSquad_OnlineError(), "lineup rejected"));
    gmSquad_OnlinePickCount(1); /* only the pick-4 line follows the count */
    assert(strstr(gmSquad_OnlineError(), "lineup rejected"));
    fixture_accept_selection = true;
    assert(!gmSquad_OnlineSelect(GMSQUAD_NONE,0) && !gmSquad_OnlineSearchReady());
    assert(strstr(gmSquad_OnlineError(), "(3/4)"));
    assert(gmSquad_OnlineSelect(2,0));
    calls = commands_sent;
    begin = clock();
    for (i=0; i<10000; ++i) assert(gmSquad_OnlineBudget());
    cached = clock() - begin;
    assert(file_opens == opens && facts_sent == facts && commands_sent - calls == 10000);
    fixture_host_support = false;
    assert(!gmSquad_OnlineBudget() && !gmSquad_OnlineSearchReady());
    assert(strstr(gmSquad_OnlineError(), "acknowledgement unavailable"));
    fixture_host_support = true;
    gmSquad_OnlineChoose(false); gmSquad_OnlineChoose(true);
    assert(gmSquad_OnlineBudget() && file_opens == opens && facts_sent == facts);
    printf("PASS: production host acknowledgement, rejected lock-in, platform floor and cached budget; "
           "first scan %u opens/%u facts %.3f ms; cached 0 opens/0 facts/1 command %.6f ms per call\n",
           opens, facts, 1000.0*scanned/CLOCKS_PER_SEC, 1000.0*cached/CLOCKS_PER_SEC/10000);
    gmSquad_OnlineChoose(false);
    {
        const int picks[] = {4, 4,0, 14,0, 18,0, 9,0};
        const int invalid[] = {4, 4,0, 26,0, 2,0, 9,0};
        const int two[] = {2, 4,0, 14,0};
        const int three[] = {3, 4,0, 14,0, 18,0};
        const int five[] = {5, 4,0, 14,0, 18,0, 9,0, 2,0}; /* never past members[] */
        const int one[] = {1, 4,0};
        /* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
         * Outside the host's launch answer the menu stand-in refuses even a valid lineup. */
        assert(gmSquad_OnlineTest(picks, 9) == -1 && !gmSquad_OnlineSearchReady());
        fixture_launch_query = 1;
        assert(gmSquad_OnlineTest(picks, 9) == 0 && gmSquad_OnlineSearchReady());
        assert(fixture_host_side.count == 4);
        assert(fixture_host_side.members[0].kind == 4 && fixture_host_side.members[1].kind == 14);
        assert(fixture_host_side.members[2].kind == 18 && fixture_host_side.members[3].kind == 9);
        assert(gmSquad_OnlineTest(invalid, 9) == -1);
        assert(gmSquad_OnlineTest(picks, 8) == -1);
        assert(gmSquad_OnlineTest(one, 3) == -1);
        assert(gmSquad_OnlineTest(two, 5) == -1);
        assert(gmSquad_OnlineTest(three, 7) == -1);
        assert(gmSquad_OnlineTest(five, 11) == -1);
        fixture_launch_query = 0;
        gmSquad_OnlineChoose(false);
    }
}
/* squadLineup: the host answers op 6 each time the online squad CSS opens; character select takes
 * the lineup once, only while the lineup can still be edited. */
static void online_agent_lineup_test(void)
{
    static const u8 valid[] = {2,3, 14,1, 18,4, 9,2};
    static const u8 bad_color[] = {2,4, 14,1, 18,4, 9,2}; /* Fox has four colours */
    MuSquadSide side;
    unsigned queries;
    fixture_host_support = true;
    gmSquad_OnlineChoose(true);
    fixture_editable = true;
    /* No lineup: one query, nothing logged, nothing to take. */
    fixture_agent_size = 0; fixture_logs[0] = 0; queries = agent_queries;
    gmSquad_OnlineCss();
    assert(agent_queries == queries + 1 && !fixture_logs[0]);
    assert(!gmSquad_OnlineAgentLineup(&side));
    /* A valid lineup is logged, refused while not editable, then taken exactly once. */
    memcpy(fixture_agent, valid, sizeof valid); fixture_agent_size = sizeof valid;
    gmSquad_OnlineCss();
    assert(!strcmp(fixture_logs, "squad: agent lineup 2:3,14:1,18:4,9:2\n"));
    fixture_editable = false;
    assert(!gmSquad_OnlineAgentLineup(&side));
    fixture_editable = true;
    assert(!gmSquad_OnlineAgentLineup(NULL));
    memset(&side, 0, sizeof side);
    assert(gmSquad_OnlineAgentLineup(&side));
    assert(side.count == 4 && side.members[0].kind == 2 && side.members[0].color == 3);
    assert(side.members[1].kind == 14 && side.members[1].color == 1);
    assert(side.members[2].kind == 18 && side.members[2].color == 4);
    assert(side.members[3].kind == 9 && side.members[3].color == 2);
    assert(!gmSquad_OnlineAgentLineup(&side));
    /* Every visit asks again, so the lineup comes back on the next CSS. */
    gmSquad_OnlineCss();
    assert(gmSquad_OnlineAgentLineup(&side) && side.count == 4);
    /* A short answer (the old three-fighter one included) or an out-of-range colour is ignored
     * and says so. */
    fixture_agent_size = 6; fixture_logs[0] = 0;
    gmSquad_OnlineCss();
    assert(!strcmp(fixture_logs, "squad: agent lineup invalid, ignored\n"));
    assert(!gmSquad_OnlineAgentLineup(&side));
    memcpy(fixture_agent, bad_color, sizeof bad_color); fixture_agent_size = sizeof bad_color;
    fixture_logs[0] = 0;
    gmSquad_OnlineCss();
    assert(!strcmp(fixture_logs, "squad: agent lineup invalid, ignored\n"));
    assert(!gmSquad_OnlineAgentLineup(&side));
    /* A stale lineup never survives a CSS visit whose host answers nothing. */
    memcpy(fixture_agent, valid, sizeof valid); fixture_agent_size = sizeof valid;
    gmSquad_OnlineCss();
    fixture_host_support = false; fixture_logs[0] = 0;
    gmSquad_OnlineCss();
    assert(!strstr(fixture_logs, "agent lineup") && !gmSquad_OnlineAgentLineup(&side));
    fixture_host_support = true; fixture_agent_size = 0; fixture_editable = false;
    gmSquad_OnlineChoose(false);
    puts("PASS: agent-test lineup asked on each squad CSS visit, taken once while editable, invalid ignored");
}
static void agent_css_reset(void)
{
    gmSquad_Clear(0);
    agent_coin_ck = GMSQUAD_NONE; agent_coin_color = 0; agent_coin_held = true;
    agent_refuse_coin = false; agent_adds = agent_coins = 0;
    mnCharSel_804D6CF2 = mnCharSel_804D6CF6 = mnCharSel_804D6CF7 = 0; mnCharSel_804D6CF0 = 0;
    HSD_PadCopyStatus[0].trigger = 0; squad_agent_wait = -1;
}
/* The CSS picks the host's lineup like a player (three members, then the coin) and presses START
 * once the fighter shows chosen, so lock-in runs the player's path. */
static void agent_css_test(void)
{
    static const u8 valid[] = {2,3, 14,1, 18,4, 9,2};
    int frame;
    fixture_host_support = true; fixture_editable = true; fixture_fill_mode = true;
    gmSquad_OnlineChoose(true);
    memcpy(fixture_agent, valid, sizeof valid); fixture_agent_size = sizeof valid;
    /* Not fill mode, or a scene change pending: the lineup waits. */
    agent_css_reset(); gmSquad_OnlineCss();
    fixture_fill_mode = false; squad_agent_lineup();
    fixture_fill_mode = true; mnCharSel_804D6CF6 = 1; squad_agent_lineup();
    assert(!agent_adds && !agent_coins && squad_agent_wait < 0);
    mnCharSel_804D6CF6 = 0;
    /* Picked in one frame, fight order kept: members then the coin's fighter last... */
    fixture_logs[0] = 0;
    squad_agent_lineup();
    assert(agent_adds == 3 && agent_coins == 1 && squad_agent_wait == 0);
    assert(gmSquad_rows[0][0].ckind == 2 && gmSquad_rows[0][0].color == 3);
    assert(gmSquad_rows[0][1].ckind == 14 && gmSquad_rows[0][1].color == 1);
    assert(gmSquad_rows[0][2].ckind == 18 && gmSquad_rows[0][2].color == 4);
    assert(agent_coin_ck == 9 && agent_coin_color == 2);
    {
        /* The frame's TrackCoin puts the coin after the three members: the host's fight order. */
        GmSquadMember order[GMSQUAD_SIZE];
        u8 ck, color;
        gmSquad_TrackCoin(0, true);
        assert(gmSquad_CssPreview(&ck, &color, order) == 4);
        for (frame = 0; frame < 4; ++frame)
            assert(order[frame].ckind == valid[2*frame] && order[frame].color == valid[2*frame + 1]);
    }
    /* ...and START only after the chosen counter is up, character select's START delay is over
     * and the wait has passed. */
    mnCharSel_804D6CF2 = 30;
    for (frame = 1; frame < 30; ++frame) {
        HSD_PadCopyStatus[0].trigger = 0;
        if (frame == 3) mnCharSel_804D6CF7 = 1;
        if (frame == 14) mnCharSel_804D6CF2 = 0;
        squad_agent_lineup();
        if (HSD_PadCopyStatus[0].trigger & HSD_PAD_START) break;
    }
    assert(frame == 14 && strstr(fixture_logs, "squad: agent lineup START 1\n"));
    /* A press that did not lock in is repeated, a bounded number of times... */
    for (frame = 1; frame <= SQUAD_AGENT_START_AGAIN; ++frame) {
        HSD_PadCopyStatus[0].trigger = 0;
        squad_agent_lineup();
        if (HSD_PadCopyStatus[0].trigger & HSD_PAD_START) break;
    }
    assert(frame == SQUAD_AGENT_START_AGAIN && strstr(fixture_logs, "squad: agent lineup START 2\n"));
    /* ...and stops as soon as lock-in took (the player's path: gmSquad_OnlineSelect). */
    agent_coin_held = false; gmSquad_TrackCoin(0, true);
    assert(gmSquad_OnlineSelect(agent_coin_ck, agent_coin_color) && gmSquad_OnlineSearchReady());
    assert(strstr(fixture_logs, "squad: locked in 4/4 2:3,14:1,18:4,9:2\n"));
    squad_agent_lineup();
    assert(squad_agent_wait < 0);
    HSD_PadCopyStatus[0].trigger = 0;
    for (frame = 0; frame < 200; ++frame) squad_agent_lineup();
    assert(!HSD_PadCopyStatus[0].trigger && agent_adds == 3 && !strstr(fixture_logs, "START 3"));
    /* A player who dropped the coin first: the old picks go and the coin is taken back. */
    agent_css_reset(); gmSquad_OnlineCss();
    gmSquad_rows[0][0] = (GmSquadMember){5,0}; agent_coin_held = false; regrabs = 0;
    squad_agent_lineup();
    assert(regrabs == 1 && regrab_door == 0 && regrab_cport == 0);
    assert(gmSquad_rows[0][0].ckind == 2 && agent_coin_ck == 9);
    /* A refused pick stops there, says which and leaves no half lineup. */
    agent_css_reset(); gmSquad_OnlineCss(); agent_refuse_coin = true; fixture_logs[0] = 0;
    squad_agent_lineup();
    assert(strstr(fixture_logs, "squad: agent lineup refused at pick 4\n") && squad_agent_wait < 0);
    assert(gmSquad_rows[0][0].ckind == GMSQUAD_NONE && gmSquad_rows[0][1].ckind == GMSQUAD_NONE);
    assert(gmSquad_rows[0][2].ckind == GMSQUAD_NONE);
    /* Character select never shows the fighter chosen: START is never pressed, and that is logged. */
    agent_css_reset(); gmSquad_OnlineCss(); fixture_logs[0] = 0;
    squad_agent_lineup();
    for (frame = 0; frame <= SQUAD_AGENT_START_WAIT; ++frame) squad_agent_lineup();
    assert(!HSD_PadCopyStatus[0].trigger && squad_agent_wait < 0);
    assert(strstr(fixture_logs,
                  "squad: agent lineup not locked in, 4/4 fighters picked, START pressed 0 times\n"));
    /* "squad: picks" only when the picks change, with the controller port. */
    agent_css_reset(); squad_logged_valid = false; mnCharSel_804D6CF0 = 1; fixture_logs[0] = 0;
    squad_log_picks(); squad_log_picks();
    assert(!strcmp(fixture_logs, "squad: picks P2 0/4\n"));
    gmSquad_rows[0][0] = (GmSquadMember){2,3}; fixture_logs[0] = 0;
    squad_log_picks(); squad_log_picks();
    assert(!strcmp(fixture_logs, "squad: picks P2 1/4 2:3\n"));
    gmSquad_rows[0][1] = (GmSquadMember){14,1}; gmSquad_rows[0][2] = (GmSquadMember){9,2};
    agent_coin_ck = 18; agent_coin_color = 4;
    gmSquad_TrackCoin(0, true);
    fixture_logs[0] = 0;
    squad_log_picks();
    assert(!strcmp(fixture_logs, "squad: picks P2 4/4 2:3,14:1,9:2,18:4\n"));
    agent_coin_color = 0; fixture_logs[0] = 0;
    squad_log_picks();
    assert(!strcmp(fixture_logs, "squad: picks P2 4/4 2:3,14:1,9:2,18:0\n"));
    agent_css_reset(); fixture_agent_size = 0;
    fixture_fill_mode = fixture_editable = false;
    gmSquad_OnlineChoose(false);
    puts("PASS: agent-test lineup picked like a player, START after the chosen wait, refusals logged, picks logged on change");
}
static void online_setup_test(void)
{
    MuSquadSide p1 = {4, {{2,0},{3,1},{5,2},{6,0}}};
    MuSquadSide p2 = {4, {{8,1},{9,2},{10,3},{11,1}}};
    MuSquadSide short_side = {3, {{2,0},{3,1},{5,2}}}, long_side = p1;
    StartMeleeData a = {0}, b = {0}, frozen;
    int i;
    for(i=0;i<128;++i) file_bytes[i] = 1024;
    for(i=40;i<80;++i) file_bytes[i] = (MU_SQUAD_ONLINE_ALLM - 0x40000 - 1280) / 4 - 256;
    s_disc_count = 0;
    for(i=0;i<GM_MAX_PLAYERS;++i) a.players[i].slot_type = b.players[i].slot_type = Gm_PKind_NA;
    s_allm_budget = 0x40000 + 1280 + 4*1280;
    s_alla_budget = 16*1280;
    major = GM_HANYU_CSS; fixture_online_chosen = true;
    long_side.count = 5; /* a fifth fighter is not a squad either */
    assert(!mu_squad_combine(0,&short_side,1,&p2,&fixture_reply));
    assert(!mu_squad_combine(0,&p1,1,&long_side,&fixture_reply));
    assert(mu_squad_combine(0,&p1,1,&p2,&fixture_reply));
    /* Everything fits: no trim and no notice. */
    for(i=40;i<80;++i) file_bytes[i] = 1024;
    s_disc_count = 0;
    gmSquad_OnlineSetup(&a);
    assert(fixture_online_ready && gmSquad_Count(0) == 3 && gmSquad_Count(1) == 3);
    assert(!gmSquad_OnlineTrimNotice()[0]);
    for(i=40;i<80;++i) file_bytes[i] = (MU_SQUAD_ONLINE_ALLM - 0x40000 - 1280) / 4 - 256;
    s_disc_count = 0;
    gmSquad_rows[0][0] = (GmSquadMember){25,4}; /* ignored local CSS */
    gmSquad_OnlineSetup(&a);
    /* Four main files fit: 4+4 trims from the back of each side, to 2+2, and says so. */
    assert(fixture_online_ready && a.players[0].stocks == 2 && a.players[1].stocks == 2);
    assert(gmSquad_Count(0) == 1 && gmSquad_Count(1) == 1);
    assert(gmSquad_rows[0][0].ckind == 3 && gmSquad_rows[1][0].ckind == 9);
    assert(!strcmp(gmSquad_OnlineTrimNotice(),
                   "Squads too big to load together: each side plays 2 fighters."));
    assert(mu_squad_combine(1,&p2,0,&p1,&fixture_reply));
    gmSquad_rows[0][0] = (GmSquadMember){20,2}; /* another local CSS */
    // Different local heap sizes must never affect online admission/trim.
    s_allm_budget *= 2; s_alla_budget *= 2;
    gmSquad_OnlineSetup(&b);
    assert(!memcmp(&a,&b,sizeof a)); /* includes admission/trim, ports, rules and stocks */
    assert(!strcmp(gmSquad_OnlineTrimNotice(),
                   "Squads too big to load together: each side plays 2 fighters."));
    gmSquad_OnEnterVs(&a); assert(gmSquad_Live() && fixture_replay_squad); frozen = a;
    a.players[0].ckind = 25; a.players[1].color = 0;
    a.players[0].stocks = a.players[1].stocks = 4; /* online base block overwritten */
    gmSquad_ApplyLiveStart(&a); assert(!memcmp(&a,&frozen,sizeof a));
    fixture_reply_valid = false;
    gmSquad_OnlineSetup(&a); assert(!fixture_online_ready && !gmSquad_Any());
    assert(!gmSquad_OnlineTrimNotice()[0]); /* an agreed normal 1v1 has nothing to say */
    gmSquad_rows[0][0] = (GmSquadMember){25,4}; frozen = a;
    gmSquad_OnEnterVs(&a); assert(!fixture_replay_squad && !gmSquad_Live() && !memcmp(&a,&frozen,sizeof a));
    fixture_reply_valid = true;
    assert(mu_squad_combine(0,&p1,1,&p2,&fixture_reply));
    /* Only two main files fit now: setup trims both exchanged squads to their coin
     * picks, and a 1-member versus 1-member squad is still a squad match. */
    for(i=40;i<80;++i) file_bytes[i] = (MU_SQUAD_ONLINE_ALLM - 0x40000 - 1280) / 2 - 256;
    s_disc_count = 0;
    major = GM_DEBUG_VS; /* a lobby launch is debug VS too: only the harness may take the squad */
    gmSquad_OnlineSetup(&a); gmSquad_OnEnterVs(&a);
    assert(!gmSquad_Live() && !fixture_replay_squad);
    fixture_harness = 1;
    gmSquad_OnlineSetup(&a); gmSquad_OnEnterVs(&a);
    assert(gmSquad_Live() && a.players[0].stocks == 1 && a.players[1].stocks == 1);
    assert(!strcmp(gmSquad_OnlineTrimNotice(),
                   "Squads too big to load together: each side plays 1 fighter."));
    fixture_online_chosen = fixture_online_ready = false; major = GM_VS; fixture_harness = 0;
    gmSquad_Disable(); gmSquad_ClearAll();
    puts("PASS: production game canonical exchanged setup, identical trim in both orders, overwrite restore and normal fallback");
}
/* Costumes are cosmetics: any two skin packs give the same identity and the same trim, a
 * non-costume file still changes the identity, and a local skin bigger than its fighter's
 * constant has to fit this machine's own heap above the online floor. */
static void costume_identity(u64* identity)
{
    s_disc_count = 0; budget_scanned = false;
    assert(gmSquad_OnlineBudget());
    *identity = fixture_identity;
}
static void costume_pack(u32 bytes)
{
    int i;
    for (i = 200; i < 640; ++i) file_bytes[i] = bytes + (u32) (i % 7) * 4096u;
}
/* The smallest main budget that takes this lineup offline (the animation budget unlimited). */
static u32 offline_need(const GmSquadMember* m, int n)
{
    u32 lo = 0, hi = 0x7FFFFFFFu;
    s_alla_budget = 0x7FFFFFFFu;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2;
        s_allm_budget = mid; s_disc_count = 0;
        if (gmSquad_Budget(m, n, 0)) hi = mid; else lo = mid + 1;
    }
    return lo;
}
static void online_costume_test(void)
{
    MuSquadSide p1 = {4, {{2,0},{3,1},{5,2},{6,0}}};
    MuSquadSide p2 = {4, {{4,1},{9,2},{10,3},{11,1}}}; /* Kirby copies 3, the colour hat kind */
    StartMeleeData a = {0}, b = {0};
    u64 vanilla, pack, other;
    u32 main_file;
    int i;
    fixture_costumes = 1; fixture_host_support = true;
    major = GM_HANYU_CSS;
    s_allm_budget = MU_SQUAD_ONLINE_ALLM; s_alla_budget = MU_SQUAD_ONLINE_ALLA;
    gmSquad_OnlineChoose(true);
    assert(gmSquad_CostumeBytes(1, GMSQUAD_FILE_COSTUME) == 362982); /* Fox: PlFxGr.dat */
    assert(gmSquad_CostumeBytes(3, GMSQUAD_FILE_HAT) == 181734);     /* PlKbBuCpDk.dat */
    assert(!gmSquad_CostumeBytes(1, GMSQUAD_FILE_HAT) && !gmSquad_CostumeBytes(33, GMSQUAD_FILE_COSTUME));
    assert(!gmSquad_CostumeBytes(1, GMSQUAD_FILE_MAIN) && !gmSquad_CostumeBytes(-1, GMSQUAD_FILE_COSTUME));
    /* Identity: the vanilla-sized disc and a pack of smaller, differently sized skins agree. */
    costume_pack(30000);
    costume_identity(&vanilla);
    costume_pack(1000);
    file_bytes[200 + 2 * 6 + 0] = 77777;
    costume_identity(&pack);
    assert(pack == vanilla);
    file_bytes[42] = 2048; /* a fighter's own data file is gameplay: a different pool */
    costume_identity(&other);
    assert(other != vanilla);
    file_bytes[42] = 1024;
    /* Trim: two skin packs trim the same exchanged squads identically, in either port order. */
    main_file = (MU_SQUAD_ONLINE_ALLM - 0x40000 - 1280) / 6;
    for (i = 40; i < 80; ++i) file_bytes[i] = main_file;
    for (i = 0; i < GM_MAX_PLAYERS; ++i) a.players[i].slot_type = b.players[i].slot_type = Gm_PKind_NA;
    assert(mu_squad_combine(0,&p1,1,&p2,&fixture_reply));
    costume_pack(1000); s_disc_count = 0;
    gmSquad_OnlineSetup(&a);
    assert(fixture_online_ready && gmSquad_Count(0) + gmSquad_Count(1) < 4); /* the costumes trimmed it */
    costume_pack(300000); s_disc_count = 0;
    gmSquad_OnlineSetup(&b);
    assert(!memcmp(&a, &b, sizeof a));
    costume_pack(900000); s_disc_count = 0; /* bigger than every vanilla costume */
    gmSquad_OnlineSetup(&b);
    assert(!memcmp(&a, &b, sizeof a));
    assert(mu_squad_combine(1,&p2,0,&p1,&fixture_reply));
    gmSquad_OnlineSetup(&b);
    assert(!memcmp(&a, &b, sizeof a));
    for (i = 40; i < 80; ++i) file_bytes[i] = 1024;
    /* Skins never refuse the search: any size, on the floor heap, and a costume the disc lacks.
     * In the match the host opens the disc's copy for any file over its size (squad_costume_rule.h). */
    costume_pack(900000);
    fixture_missing = 200 + 1 * 6 + 1;
    s_allm_budget = MU_SQUAD_ONLINE_ALLM;
    assert(gmSquad_OnlineBudget() && !gmSquad_OnlineError()[0]);
    fixture_missing = -1;
    /* Only a host whose own standard copy is over its size answers 2: refused, saying why, on a
     * fresh scan and on the cached identity alike; a later 1 clears it. */
    fixture_budget_answer = 2;
    assert(!gmSquad_OnlineBudget() && !gmSquad_OnlineSearchReady());
    assert(!strcmp(gmSquad_OnlineError(), "This disc's own costumes are too big for Squad Strike online. Search refused."));
    budget_scanned = false; s_disc_count = 0;
    assert(!gmSquad_OnlineBudget() && strstr(gmSquad_OnlineError(), "disc's own costumes"));
    /* 3: the standard copy over its size is a mod profile's (its own view): named as the mod's. */
    fixture_budget_answer = 3;
    assert(!gmSquad_OnlineBudget() && !gmSquad_OnlineSearchReady());
    assert(!strcmp(gmSquad_OnlineError(), "This mod's costumes are too big for Squad Strike online. Search refused."));
    fixture_budget_answer = 0;
    assert(!gmSquad_OnlineBudget() && strstr(gmSquad_OnlineError(), "budget identity unavailable"));
    fixture_budget_answer = 1;
    assert(gmSquad_OnlineBudget() && !gmSquad_OnlineError()[0]);
    for (i = 0; i < 640; ++i) file_bytes[i] = 1024;
    s_allm_budget = MU_SQUAD_ONLINE_ALLM; s_disc_count = 0; budget_scanned = false;
    fixture_online_ready = false; major = GM_VS;
    gmSquad_OnlineChoose(false);
    /* Offline (one machine, nothing exchanged): a costume costs its local length exactly, as
     * before the constants, never max(constant, local): Captain Falcon's 558539-byte constant
     * does not apply, the need moves with the skin byte for byte, and a costume the disc lacks
     * is unaffordable. */
    {
        const GmSquadMember m[1] = {{2, 0}}; /* costume file 200 + 2 * 6 + 0 */
        u32 base, small, big;
        fixture_costumes = 0;
        base = offline_need(m, 1);
        fixture_costumes = 1;
        costume_pack(1000);
        small = offline_need(m, 1);
        assert(small - base == ((file_bytes[212] + 31u) & ~31u) + 0x100u);
        assert(small - base < gmSquad_CostumeBytes(2, GMSQUAD_FILE_COSTUME));
        costume_pack(901000);
        big = offline_need(m, 1);
        assert(big - small == 900000);
        fixture_missing = 212;
        s_allm_budget = 0x7FFFFFFEu; s_disc_count = 0;
        assert(!gmSquad_Budget(m, 1, 0));
        fixture_missing = -1;
    }
    for (i = 0; i < 640; ++i) file_bytes[i] = 1024;
    s_allm_budget = MU_SQUAD_ONLINE_ALLM; s_alla_budget = MU_SQUAD_ONLINE_ALLA; s_disc_count = 0;
    fixture_costumes = 0;
    gmSquad_Disable(); gmSquad_ClearAll();
    puts("PASS: costumes keep the identity and the trim for any skin pack; skins never refuse, only a standard copy over its size does; offline charges the local length");
}
/* Friends: the synced select runs this offline engine on both machines (a console session), so
 * squad_add, squad_place_coin and squad_trim must decide alike on each. Each machine's own heap and
 * its own skins' lengths must not matter: the fixed online floors and the costume constants do. */
static bool friends_decide(const GmSquadMember* m, int n, u32 allm, u32 alla, u32 pack)
{
    s_allm_budget = allm; s_alla_budget = alla;
    costume_pack(pack); s_disc_count = 0;
    return gmSquad_Budget(m, n, 0);
}
static void friends_fixed_budget_test(void)
{
    const GmSquadMember m[6] = {{2,0},{3,1},{5,2},{6,0},{8,1},{9,2}};
    /* Local heaps: under both floors, 32 bytes under one or the other, on them, far over. */
    const u32 heaps[5][2] = {{0x40000, 0x1000}, {MU_SQUAD_ONLINE_ALLM - 32u, MU_SQUAD_ONLINE_ALLA},
                             {MU_SQUAD_ONLINE_ALLM, MU_SQUAD_ONLINE_ALLA - 32u},
                             {MU_SQUAD_ONLINE_ALLM, MU_SQUAD_ONLINE_ALLA}, {0x7FFFFFF0u, 0x7FFFFFF0u}};
    const u32 packs[3] = {1000, 300000, 900000};
    u32 main_file, lo, hi;
    int i, h, k, n, checked = 0;
    fixture_costumes = 1; fixture_online_chosen = false; major = GM_VS;
    for (i = 0; i < 640; ++i) file_bytes[i] = 1024;
    /* Off a console session the local heap decides: a tiny heap refuses, a big one admits. */
    fixture_console = 0;
    assert(!friends_decide(m, 2, 0x40000, 0x1000, 1000));
    assert(friends_decide(m, 2, 0x7FFFFFF0u, 0x7FFFFFF0u, 1000));
    /* In it the floors decide: the same answer on the floor heap and a huge one, with any skin
     * pack, for every lineup size, and with main files around the floor's edge. A machine whose
     * own heap is under either floor refuses squads (as online refuses its search). */
    fixture_console = 1;
    for (n = 1; n <= 6; ++n) {
        /* The largest main file size this lineup takes on the floors (binary search). */
        lo = 0; hi = MU_SQUAD_ONLINE_ALLM;
        while (lo < hi) {
            const u32 mid = lo + (hi - lo + 1) / 2;
            for (i = 40; i < 80; ++i) file_bytes[i] = mid;
            if (friends_decide(m, n, MU_SQUAD_ONLINE_ALLM, MU_SQUAD_ONLINE_ALLA, 1000)) lo = mid; else hi = mid - 1;
        }
        assert(lo > 0 && lo < MU_SQUAD_ONLINE_ALLM);
        for (main_file = lo; main_file <= lo + 1; ++main_file) {
            bool want, floor_answer;
            for (i = 40; i < 80; ++i) file_bytes[i] = main_file;
            fixture_online_chosen = true; fixture_console = 0;
            floor_answer = friends_decide(m, n, MU_SQUAD_ONLINE_ALLM, MU_SQUAD_ONLINE_ALLA, 1000);
            fixture_online_chosen = false; fixture_console = 1;
            want = friends_decide(m, n, heaps[3][0], heaps[3][1], packs[0]);
            assert(want == floor_answer && want == (main_file == lo)); /* admitted at the edge only */
            for (h = 0; h < 5; ++h)
                for (k = 0; k < 3; ++k)
                    assert(friends_decide(m, n, heaps[h][0], heaps[h][1], packs[k]) == (h >= 3 && want));
            ++checked;
        }
    }
    assert(checked == 12);
    fixture_console = 0;
    for (i = 0; i < 640; ++i) file_bytes[i] = 1024;
    s_allm_budget = MU_SQUAD_ONLINE_ALLM; s_alla_budget = MU_SQUAD_ONLINE_ALLA; s_disc_count = 0;
    fixture_costumes = 0;
    puts("PASS: Friends synced select budgets on the fixed floors and costume constants, never this host's skins; a host under the floors refuses");
}
/* The worst vanilla 4v4 by animation, found over every fighter. Each CSS fighter loads its own
 * animation file and its partner's (Ice Climbers: Popo's PlPpAJ.dat and Nana's PlNnAJ.dat,
 * ftnana.c; Zelda and Sheik: both of theirs), at the GALE01 rev 2 disc's lengths, each rounded to 32
 * plus the archive's 256. The worst eight distinct fighters are Kirby, Link, Marth, Ness, Peach,
 * Zelda (with Sheik's), Young Link and Roy: 16590272 bytes, over every 32 MiB boot, and within the
 * 40 MiB floor on every machine. Ice Climbers need 1212192. */
static const u32 worst_aj[27] = { /* internal kinds: Mr Fx Ca Dk Kb Kp Lk Sk Ns Pe Pp Nn Pk Ss ... */
    1259328, 1525984, 1633440, 1599360, 1721600, 1640768, 1691040, 1366336, 1691968, 2102080,
    1164448, 47232, 1303584, 1443136, 1493088, 1087072, 1352896, 1334304, 1942752, 2182400,
    1862592, 1280960, 1495328, 1045312, 1139968, 1649216, 2027200};
/* CharacterKind -> internal kind and partner (ftMapping_list), -1 for none. */
static const s8 worst_fighter[26][2] = {
    {2,-1},{3,-1},{1,-1},{24,-1},{4,-1},{5,-1},{6,-1},{17,-1},{0,-1},{18,-1},{16,-1},{8,-1},{9,-1},
    {12,-1},{10,11},{15,-1},{13,-1},{14,-1},{19,7},{7,19},{22,-1},{20,-1},{21,-1},{26,-1},{23,-1},{25,-1}};
static u32 worst_files_bytes(u32 files)
{
    u32 total = 0;
    int i;
    for (i = 0; i < 27; ++i)
        if (files >> i & 1u) total += ((worst_aj[i] + 31u) & ~31u) + 0x100u;
    return total;
}
static u32 worst_fighter_files(int ck)
{
    return (1u << worst_fighter[ck][0]) | (worst_fighter[ck][1] >= 0 ? 1u << worst_fighter[ck][1] : 0u);
}
/* Every set of eight distinct fighters (26 choose 8): the largest animation total and its set. */
static u32 worst_search(int from, int left, u32 files, u32* best, u32* best_set, u32 set)
{
    int ck;
    if (left == 0) {
        const u32 bytes = worst_files_bytes(files);
        if (bytes > *best) { *best = bytes; *best_set = set; }
        return 1;
    } else {
        u32 sets = 0;
        for (ck = from; ck <= 26 - left; ++ck)
            sets += worst_search(ck + 1, left - 1, files | worst_fighter_files(ck), best, best_set, set | 1u << ck);
        return sets;
    }
}
static void worst_4v4_fits_test(void)
{
    const u32 heaps[4][2] = {{0x40000, 0x1000}, {0xC96800, 13313952},
                             {MU_SQUAD_ONLINE_ALLM, MU_SQUAD_ONLINE_ALLA}, {0x7FFFFFF0u, 0x7FFFFFF0u}};
    const u32 packs[3] = {1000, 300000, 900000};
    GmSquadMember m[8];
    u32 need = 0, set = 0, sets, margin, extras[3], fighter_bytes[26];
    int i, e, h, k, n = 0, mode, largest = 0;
    /* Squad hosts back 40 MiB of ARAM. The game's arena starts where the measured boots put it
     * (retail 17094752, mod assets 2 MiB later), so AllA is 23799712 and 21702560 bytes, both over
     * the floor; the old 32 MiB gave 15411104 and 13313952, under the worst 4v4. */
    _Static_assert(MU_HOST_ARAM_BYTES == 40u << 20, "squad hosts back 40 MiB of ARAM");
    _Static_assert(MU_SQUAD_ONLINE_ALLA == 0x1100000u, "the animation floor covers the worst 4v4");
    assert(mu_squad_animation_heap(0x96C800, 17094752, MU_HOST_ARAM_BYTES) == 23799712);
    assert(mu_squad_animation_heap(0x96C800, 19191904, MU_HOST_ARAM_BYTES) == 21702560);
    assert(mu_squad_animation_heap(0x96C800, 17094752, 32u << 20) == 15411104);
    assert(mu_squad_animation_heap(0x96C800, 19191904, 32u << 20) == 13313952);
    /* The per-fighter table: Ice Climbers carry Nana's file, Zelda and Sheik share both. */
    for (i = 0; i < 26; ++i) {
        fighter_bytes[i] = worst_files_bytes(worst_fighter_files(i));
        if (fighter_bytes[i] > fighter_bytes[largest]) largest = i;
    }
    assert(fighter_bytes[14] == 1212192u && fighter_bytes[14] > worst_files_bytes(1u << 10));
    assert(fighter_bytes[18] == 3549248u && fighter_bytes[19] == fighter_bytes[18] && largest == 18);
    sets = worst_search(0, 8, 0, &need, &set, 0);
    assert(sets == 1562275u);
    assert(need == 16590272u);
    assert(set == (1u<<4 | 1u<<6 | 1u<<9 | 1u<<11 | 1u<<12 | 1u<<18 | 1u<<21 | 1u<<23));
    margin = MU_SQUAD_ONLINE_ALLA - need;
    assert(margin == 1235520u && 21702560u >= MU_SQUAD_ONLINE_ALLA && 13313952u < need);
    extras[0] = 0; extras[1] = margin; extras[2] = margin + 32u;
    /* The same set through gmSquad_Budget. The fixture loads file `kind` as a fighter's animation
     * and file 19 as Zelda's partner (Sheik's), so these lengths are exactly the set's. */
    fixture_costumes = 1; fixture_online_chosen = false; fixture_console = 0; major = GM_VS;
    for (i = 0; i < 640; ++i) file_bytes[i] = 1024;
    for (i = 0; i < 26; ++i) {
        if (!(set >> i & 1u)) continue;
        m[n].ckind = (u8) i; m[n].color = (u8) (n & 3); ++n;
        file_bytes[i] = worst_aj[worst_fighter[i][0]];
        if (worst_fighter[i][1] >= 0) file_bytes[i == 18 ? 19 : 15] = worst_aj[worst_fighter[i][1]];
    }
    assert(n == 8 && !(set >> 14 & 1u) && !(set >> 15 & 1u) && !(set >> 19 & 1u));
    /* Offline the local heap decides: the old 32 MiB mod-asset AllA trimmed it, 40 MiB keeps it. */
    assert(!friends_decide(m, 8, 0xC96800, 13313952, 1000));
    assert(friends_decide(m, 8, 0xC96800, 21702560, 1000));
    assert(friends_decide(m, 8, 0xC96800, 23799712, 1000));
    /* Friends (a console session) and online: the floors decide, so every machine at or over them
     * admits it with any skin pack; one Roy file 1235520 bytes longer still fits exactly, and 32 more
     * is refused everywhere. A Friends machine under the floors refuses squads outright. */
    for (mode = 0; mode < 2; ++mode) {
        fixture_console = mode == 0; fixture_online_chosen = mode == 1;
        for (e = 0; e < 3; ++e) {
            const bool fits = extras[e] <= margin;
            file_bytes[23] = 2027200u + extras[e];
            for (h = 0; h < 4; ++h)
                for (k = 0; k < 3; ++k)
                    assert(friends_decide(m, 8, heaps[h][0], heaps[h][1], packs[k]) ==
                           (fits && (mode == 1 || h >= 2)));
        }
    }
    fixture_console = 0; fixture_online_chosen = false;
    for (i = 0; i < 640; ++i) file_bytes[i] = 1024;
    s_allm_budget = MU_SQUAD_ONLINE_ALLM; s_alla_budget = MU_SQUAD_ONLINE_ALLA; s_disc_count = 0;
    fixture_costumes = 0;
    puts("PASS: the worst vanilla 4v4 (16590272 animation bytes) fits the 40 MiB floor on every machine");
}
static unsigned cosmetics_counted;
static void count_cosmetic(void* ctx, const char* name, int use)
{ (void) ctx; (void) name; if (use == GMSQUAD_FILE_COSTUME || use == GMSQUAD_FILE_HAT) ++cosmetics_counted; }
/* Friends' synced select charges costumes at their constants on a main heap with no room over them,
 * so while squads are in play in a console session the host's disc-costume rule is armed: before
 * every preload list, facts first when the host has none, off once squads or the session end. */
static void console_costume_rule_test(void)
{
    const GmSquadMember one[1] = {{5, 1}};
    struct GameCache c;
    int ck, color, target;
    memset(&c, 0, sizeof c);
    for (ck = 0; ck < 8; ++ck) c.entries[ck].char_id = ChKind_None;
    c.entries[0].char_id = 2; c.entries[1].char_id = 3;
    for (ck = 0; ck < 640; ++ck) file_bytes[ck] = 1024;
    fixture_costumes = 1; fixture_online_chosen = false; major = GM_VS; allowed = 1;
    gmSquad_ClearAll(); gmSquad_Disable();
    console_facts = console_arms = console_offs = 0; console_have_facts = false; fixture_console_answer = 1;
    /* Every fighter's costumes and every Kirby hat, as the identity scan visits them. */
    cosmetics_counted = 0;
    for (ck = 0; ck < 26; ++ck)
        for (color = 0; color < (int) mu_squad_colors((unsigned) ck); ++color) {
            s32 extra, kind = mu_squad_fighter_kinds(ck, &extra);
            mu_squad_fighter_files(kind, (u8) color, NULL, count_cosmetic);
            mu_squad_fighter_files(extra, (u8) color, NULL, count_cosmetic);
        }
    for (color = 0; color < (int) mu_squad_colors(CKind_Kirby); ++color)
        for (target = 0; target < 26; ++target) {
            s32 extra, kind = mu_squad_fighter_kinds(target, &extra);
            mu_squad_copy_files(kind, (u8) color, NULL, count_cosmetic);
            mu_squad_copy_files(extra, (u8) color, NULL, count_cosmetic);
        }
    assert(cosmetics_counted > 26);
    /* Offline nothing is asked; a console session with no squad in play asks nothing either (no
     * rule armed, nothing to switch off). */
    fixture_console = 0; gmSquad_AddPreloads(&c);
    assert(!console_arms && !console_offs && !console_facts);
    fixture_console = 1; gmSquad_AddPreloads(&c); gmSquad_AddPreloads(&c);
    assert(!console_arms && !console_offs && !console_facts);
    /* A squad pick arms it: the host has no facts, so the game sends them, then arms again. */
    gmSquad_rows[0][0] = (GmSquadMember){5, 1};
    gmSquad_AddPreloads(&c);
    assert(console_arms == 2 && console_facts == cosmetics_counted && console_have_facts);
    assert(gmSquad_Budget(one, 1, 0));
    /* Held facts: the next list only arms. Stock rules only: another mode turns it off. */
    gmSquad_AddPreloads(&c);
    assert(console_arms == 3 && console_facts == cosmetics_counted);
    allowed = 0; gmSquad_AddPreloads(&c); assert(console_offs == 1 && !console_have_facts);
    gmSquad_AddPreloads(&c); assert(console_offs == 1);   /* off once */
    allowed = 1;
    /* A standard copy over its size (never the vanilla disc): the session's squads are refused,
     * after the squads are cleared too, until the session ends; then offline decides as before. */
    fixture_console_answer = 2;
    gmSquad_AddPreloads(&c);
    assert(console_arms == 5 && !gmSquad_Budget(one, 1, 0));
    gmSquad_ClearAll(); gmSquad_AddPreloads(&c);
    assert(console_offs == 2 && !gmSquad_Budget(one, 1, 0));
    fixture_console = 0; s_allm_budget = s_alla_budget = 0x7FFFFFF0u;
    gmSquad_AddPreloads(&c);
    assert(console_offs == 2 && gmSquad_Budget(one, 1, 0));
    /* Leaving the session while armed sends off once. */
    fixture_console = 1; fixture_console_answer = 1; gmSquad_rows[0][0] = (GmSquadMember){5, 1};
    gmSquad_AddPreloads(&c); assert(console_arms == 7);
    fixture_console = 0; gmSquad_AddPreloads(&c); gmSquad_AddPreloads(&c);
    assert(console_offs == 3 && console_arms == 7);
    gmSquad_ClearAll(); gmSquad_Disable();
    s_allm_budget = MU_SQUAD_ONLINE_ALLM; s_alla_budget = MU_SQUAD_ONLINE_ALLA; s_disc_count = 0;
    fixture_costumes = 0; preload_fighters = preload_copies = 0;
    puts("PASS: Friends squads arm the disc-costume rule, facts first, off when squads or the session end");
}
#endif

static void add(int port, int index, int ck, int color)
{ gmSquad_rows[port][index] = (GmSquadMember){ (u8)ck, (u8)color }; }

_Static_assert(GMSQUAD_SIZE == 4 && GMSQUAD_EXTRA == 3, "a squad is the coin pick plus three extras");

int main(void)
{
    GmSquadMember order[GMSQUAD_SIZE];
    StartMeleeData start = {0}, before;
    struct GameCache cache = {0};
    f32 icon;
    int p, old_warms, old_copies;
    for (p = 0; p < 640; ++p) { snprintf(file_names[p], 16, "%d", p); file_bytes[p] = 1024; }
    mu_squad_main_heap(0x64B400, 0, 40u << 20);
    mu_squad_animation_heap(0x96C800, 0, 20u << 20);
    gmSquad_ClearAll();
    assert(!gmSquad_Any() && gmSquad_StocksFor() == 1);
    assert(gmSquad_Lineup(-1, 2, 0, order) == 0);
    assert(gmSquad_Lineup(4, 2, 0, order) == 0);
    assert(gmSquad_Count(-1) == 0 && gmSquad_Count(4) == 0);

    /* Pick before the coin, drop it, then pick after it. */
    add(0, 0, 9, 2);
    gmSquad_TrackCoin(0, true);
    add(0, 1, 2, 1);
    assert(gmSquad_Lineup(0, 7, 3, order) == 3);
    assert(order[0].ckind == 9 && order[1].ckind == 7 && order[2].ckind == 2);
    assert(order[0].color == 2 && order[1].color == 3 && order[2].color == 1);
    gmSquad_TrackCoin(0, false);
    assert(gmSquad_Lineup(0, GMSQUAD_NONE, 0, order) == 2);
    gmSquad_TrackCoin(0, true);
    assert(gmSquad_Lineup(0, 7, 0, order) == 3 && order[2].ckind == 7);
    gmSquad_rows[0][1].ckind = GMSQUAD_NONE;
    gmSquad_TrackCoin(0, true);
    assert(gmSquad_coin_pos[0] == 1);
    add(1, 0, 14, 1); add(1, 1, 18, 2); add(1, 2, 2, 0);
    assert(gmSquad_StocksFor() == 4 && gmSquad_StocksFor() == GMSQUAD_SIZE);
    assert(gmSquad_Count(1) == GMSQUAD_EXTRA);
    { u32 key = gmSquad_Key(); add(1, 1, 18, 3); assert(gmSquad_Key() != key); }
    gmSquad_ClearAll();

    for (p = 0; p < GM_MAX_PLAYERS; ++p) start.players[p].slot_type = Gm_PKind_NA;
    start.rules.match_kind = MatchKind_Stock;
    start.players[0] = (PlayerInitData){4, 0, Gm_PKind_Human, 7};
    start.players[1] = (PlayerInitData){2, 1, Gm_PKind_Cpu, 7};
    before = start;
    gmSquad_OnEnterVs(&start);
    assert(!gmSquad_Live() && !memcmp(&before, &start, sizeof start));
    add(0, 0, 14, 1); add(0, 1, 18, 2);
    gmSquad_TrackCoin(0, true); /* ICs, Zelda, then Kirby */
    allowed = 0; gmSquad_OnEnterVs(&start);
    assert(!gmSquad_Live() && !memcmp(&before, &start, sizeof start));
    allowed = 1; start.rules.is_teams = 1; before = start;
    gmSquad_OnEnterVs(&start);
    assert(!gmSquad_Live() && !memcmp(&before, &start, sizeof start));
    start.rules.is_teams = 0;
    start.players[2].slot_type = Gm_PKind_Human; before = start;
    gmSquad_OnEnterVs(&start);
    assert(!gmSquad_Live() && !memcmp(&before, &start, sizeof start));
    start.players[2].slot_type = Gm_PKind_NA;
    gmSquad_OnEnterVs(&start);
    assert(gmSquad_Live() && start.players[0].ckind == 14);
    assert(start.players[0].stocks == 3 && start.players[1].stocks == 3); /* one per fighter: Kirby + two */
    for (p = 0; p < 4; ++p) {
        players[p].pkind = p < 2 ? Gm_PKind_Human : Gm_PKind_NA;
        players[p].player_state = 2;
        players[p].stocks = 3;
    }
    gmSquad_WarmFiles(); /* four members (3 + 1), including Nana and Sheik */
    assert(warms == 6 * 4 && copy_warms == 1);
    old_warms = warms; old_copies = copy_warms;
    assert(gmSquad_StockIconFrame(0, 1, &icon) && icon == 182);
    assert(!gmSquad_StockIconFrame(0, 0, &icon));
    assert(!gmSquad_HoldRespawn(0, 0));
    players[0].stocks = 2;
    assert(!gmSquad_HoldRespawn(0, 1));
    assert(gmSquad_HoldRespawn(0, 0));
    gmSquad_OnVsFrame(); /* same body: do not free a dying callback */
    assert(!frees && !respawns);
    ++frame; gmSquad_OnVsFrame();
    assert(frees == 1 && creates == 1 && respawns == 1);
    assert(players[0].ckind == 18 && players[0].color == 2);
    assert(players[0].transformed[0] == 0 && players[0].transformed[1] == 1);
    assert(!gmSquad_HoldRespawn(0, 0));
    players[0].stocks = 1;
    assert(gmSquad_HoldRespawn(0, 0));
    ++frame; gmSquad_OnVsFrame();
    assert(players[0].ckind == 4 && frees == 2);
    assert(warms == old_warms && copy_warms == old_copies); /* no swap I/O */
    players[0].stocks = 0;
    assert(!gmSquad_HoldRespawn(0, 0));
    gmSquad_Disable(); assert(!gmSquad_Live());

    /* Replacing a transformed/same-kind fighter still consumes the next
     * lineup entry, and an incoming Nana is put to sleep before revival. */
    gmSquad_ClearAll();
    start.players[0].ckind = 18; start.players[0].color = 0;
    add(0, 0, 14, 1);
    gmSquad_coin_pos[0] = 0;
    gmSquad_OnEnterVs(&start);
    players[0].stocks = 1;
    players[0].ckind = 19; /* Sheik when Zelda's stock was lost */
    assert(gmSquad_HoldRespawn(0, 0));
    ++frame; gmSquad_OnVsFrame();
    assert(players[0].ckind == 14 && nana_sleeps == 1);
    gmSquad_ClearAll();
    add(0, 0, 18, 2); /* same kind as the coin, different color */
    gmSquad_coin_pos[0] = 0;
    start.players[0].ckind = 18;
    gmSquad_OnEnterVs(&start);
    players[0].ckind = 18; players[0].stocks = 1;
    assert(gmSquad_HoldRespawn(0, 0));
    ++frame; gmSquad_OnVsFrame();
    assert(players[0].ckind == 18 && players[0].color == 2);
    gmSquad_Disable();

    /* Same fighter and costume on both sides: the later port is tinted from the start,
     * and a swap re-runs the rule for whoever is on stage now, both fighters included. */
    {
        int was_kind = players[1].ckind, was_color = players[1].color, was_slot = players[1].pkind;
        HSD_GObj* was_a = players[1].player_entity[0];
        HSD_GObj* was_b = players[1].player_entity[1];
        gmSquad_ClearAll();
        start.players[0].ckind = 2; start.players[0].color = 1; start.players[0].sub_color = 3;
        start.players[1].ckind = 2; start.players[1].color = 1; start.players[1].sub_color = 3;
        add(0, 0, 9, 0);
        gmSquad_coin_pos[0] = 0;
        gmSquad_OnEnterVs(&start);
        assert(start.players[0].sub_color == 0 && start.players[1].sub_color == 1);
        players[1].pkind = Gm_PKind_Human; players[1].ckind = 9; players[1].color = 0;
        players[1].player_entity[0] = &entities[1][0];
        players[1].player_entity[1] = NULL;
        fixture_fighters[1][0].sub_color = 0; fixture_fighters[0][0].sub_color = 2;
        fixture_fighters[0][0].x610_color_rgba[0] = (GXColor){9, 9, 9, 9};
        fixture_fighters[1][0].x610_color_rgba[0] = (GXColor){9, 9, 9, 9};
        fixture_sub_color[0] = fixture_sub_color[1] = 2;
        players[0].ckind = 2; players[0].color = 1; players[0].stocks = 1;
        assert(gmSquad_HoldRespawn(0, 0));
        ++frame; gmSquad_OnVsFrame();
        assert(players[0].ckind == 9 && players[0].color == 0);
        assert(fixture_sub_color[0] == 0 && fixture_sub_color[1] == 1);
        assert(fixture_fighters[0][0].sub_color == 0 && fixture_fighters[1][0].sub_color == 1);
        assert(fixture_fighters[0][0].x610_color_rgba[0].a == 0 && fixture_fighters[0][0].x610_color_rgba[0].r == 0);
        assert(fixture_fighters[1][0].x610_color_rgba[0].r == 51 && fixture_fighters[1][0].x610_color_rgba[0].g == 0 &&
               fixture_fighters[1][0].x610_color_rgba[0].a == 51);
        gmSquad_Disable();
        players[1].ckind = was_kind; players[1].color = was_color; players[1].pkind = was_slot;
        players[1].player_entity[0] = was_a; players[1].player_entity[1] = was_b;
    }
    gmSquad_ClearAll(); add(0, 0, 14, 1); add(0, 1, 18, 2);

    /* Empty CSS preloads may not retain last match's squad files. */
    for (p = 0; p < 8; ++p) cache.entries[p].char_id = ChKind_None;
    gmSquad_AddPreloads(&cache); assert(!preload_fighters);
    cache.entries[0].char_id = CKind_Kirby;
    cache.entries[1].char_id = 2;
    gmSquad_AddPreloads(&cache);
    assert(preload_fighters == 2 && preload_copies == 3);
    /* Heap growth fits both measured 40 MiB layouts, and falls back
     * with a smaller arena, reversed bounds, or insufficient main margin. */
    assert(mu_squad_main_heap(0x64B400, 0, 34647968) == 0xC96800);
    assert(mu_squad_main_heap(0x64B400, 0, 32550816) == 0xC96800);
    assert(mu_squad_main_heap(0x64B400, 0, 24u << 20) == 0x64B400);
    assert(mu_squad_main_heap(0x64B400, 100, 99) == 0x64B400);
    assert(mu_squad_animation_heap(0x96C800, 0, 15411104 + 1048576) == 15411104);
    assert(mu_squad_animation_heap(0x96C800, 0, 13313952 + 1048576) == 13313952);
    assert(mu_squad_animation_heap(0x96C800, 100, 99) == 0);
    /* The online floors must stay at or below every measured boot layout (retail, mod assets);
     * AllA's are the 40 MiB ARAM layouts (worst_4v4_fits_test). */
    assert(MU_SQUAD_ONLINE_ALLA <= 21702560u && MU_SQUAD_ONLINE_ALLM <= 0xC96800u);
    { u32 banks[8] = {100,90,80,70,60,50,40,30};
      assert(mu_squad_sound_heap(400, banks) == 580); }
    { u32 banks[8] = {613088,599712,590176,586208,580128,573216,562912,526752};
      assert(mu_squad_sound_heap(3291584, banks) == 5534592); }
    /* Save/restore even when leaving Stock, and adopt a Rules edit. */
    { int saved = -1, applied = -1;
      assert(gmSquad_StockRule(true, 7, 3, &saved, &applied) == 3 && saved == 7);
      assert(gmSquad_StockRule(true, 3, 4, &saved, &applied) == 4 && saved == 7);
      assert(gmSquad_StockRule(false, 4, 0, &saved, &applied) == 7 && saved == -1);
      assert(gmSquad_StockRule(true, 5, 3, &saved, &applied) == 3);
      assert(gmSquad_StockRule(true, 8, 4, &saved, &applied) == 4 && saved == 8);
      assert(gmSquad_StockRule(false, 4, 0, &saved, &applied) == 8);
      assert(gmSquad_StockRule(true, 9, 4, &saved, &applied) == 4);
      assert(gmSquad_StockRule(false, 6, 0, &saved, &applied) == 6); }
    /* Actual production file counting: deduplicate, include extra fighters,
     * reject animation/main overflow and check Kirby copies. */
    { GmSquadMember m[3] = {{2,0},{2,0},{14,0}};
      mu_squad_main_heap(0x64B400, 0, 40u << 20);
      mu_squad_animation_heap(0, 0, 0x100000 + 4000);
      assert(gmSquad_Budget(m, 2, 0)); /* same file counted once */
      assert(gmSquad_Budget(m + 2, 1, 0)); /* Popo and Nana */
      file_bytes[15] = 10000; assert(!gmSquad_Budget(m + 2, 1, 0));
      file_bytes[15] = 1024;
      file_bytes[42] = 20u << 20; assert(!gmSquad_Budget(m, 1, 0));
      file_bytes[42] = 1024;
      m[1].ckind = 4; file_bytes[82] = 20u << 20;
      assert(!gmSquad_Budget(m, 2, 0)); file_bytes[82] = 1024;
      assert(gmSquad_Budget(m, 2, 0));
    }
    /* Setup trim removes the back of both squads in the same port order,
     * retaining the two coins and warming only the admitted lineup. */
    gmSquad_ClearAll();
    start.players[0].ckind = 1; start.players[1].ckind = 2;
    add(0, 0, 3, 0); add(0, 1, 5, 0); add(1, 0, 6, 0); add(1, 1, 7, 0);
    mu_squad_animation_heap(0, 0, 0x100000 + 6000);
    gmSquad_OnEnterVs(&start);
    assert(gmSquad_Live() && gmSquad_Count(0) == 1 && gmSquad_Count(1) == 1);
    assert(gmSquad_rows[0][0].ckind == 3 && gmSquad_rows[1][0].ckind == 6);
    assert(start.players[0].stocks == 2 && start.players[1].stocks == 2);
    gmSquad_ClearAll();
    cursors[0].x5=2;
    HSD_PadCopyStatus[0].trigger=HSD_PAD_Z;
    squad_buttons(0);
    assert(HSD_PadCopyStatus[0].trigger==HSD_PAD_Z && !ui_adds && !ui_randoms && !buzzes);
    ui_players=3;
    squad_buttons(0);
    assert(HSD_PadCopyStatus[0].trigger==HSD_PAD_Z && !buzzes);
    hover=2;
    squad_buttons(0);
    assert(!HSD_PadCopyStatus[0].trigger && ui_adds==1 && buzzes==1);
    hover=-1; random_area=1; ui_players=2;
    HSD_PadCopyStatus[0].trigger=HSD_PAD_Z;
    squad_buttons(0); assert(!HSD_PadCopyStatus[0].trigger && ui_randoms==1);
    HSD_PadCopyStatus[0].trigger=HSD_PAD_R;
    squad_buttons(0); assert(ui_randoms==1); /* R alone must never add */
    /* Online fill mode: B with the coin down (the fourth pick) brings the coin back to the
     * hand and never removes a member; with the coin in hand B removes the last member. A hold
     * that starts on a pick is the squad's (the screen's hold-B exit never sees it); from an
     * empty squad B stays the screen's. R and Z are never the squad's here. */
    gmSquad_ClearAll(); add(0,0,9,0); random_area=0; cursors[0].x5=2;
    fixture_fill_mode = true; fixture_coin_pick = 7;
    { int before = backs, adds = ui_adds, randoms = ui_randoms, buzzed = buzzes;
      HSD_PadCopyStatus[0].trigger=HSD_PAD_R; squad_buttons(0);
      assert(!regrabs && gmSquad_Count(0)==1 && backs==before && HSD_PadCopyStatus[0].trigger==HSD_PAD_R);
      random_area=1; HSD_PadCopyStatus[0].trigger=HSD_PAD_Z; squad_buttons(0);
      assert(!regrabs && gmSquad_Count(0)==1 && HSD_PadCopyStatus[0].trigger==HSD_PAD_Z);
      assert(ui_adds==adds && ui_randoms==randoms && buzzes==buzzed); /* Z: the screen's disconnect */
      random_area=0;
      HSD_PadCopyStatus[0].trigger=HSD_PAD_B; HSD_PadCopyStatus[0].button=HSD_PAD_B; squad_b_track(); squad_buttons(0);
      assert(regrabs==1 && regrab_door==0 && regrab_cport==0 && gmSquad_Count(0)==1 && backs==before+1);
      assert(!(HSD_PadCopyStatus[0].trigger & HSD_PAD_B) && !(HSD_PadCopyStatus[0].button & HSD_PAD_B));
      HSD_PadCopyStatus[0].trigger=0; HSD_PadCopyStatus[0].button=0; squad_b_track(); squad_buttons(0);
      assert(!squad_b_held[0] && !squad_b_owned[0]);
      fixture_coin_pick = GMSQUAD_NONE;
      HSD_PadCopyStatus[0].trigger=HSD_PAD_B; HSD_PadCopyStatus[0].button=HSD_PAD_B; squad_b_track(); squad_buttons(0);
      assert(regrabs==1 && gmSquad_Count(0)==0 && backs==before+2);
      HSD_PadCopyStatus[0].trigger=0; HSD_PadCopyStatus[0].button=0; squad_b_track(); squad_buttons(0);
      HSD_PadCopyStatus[0].trigger=HSD_PAD_B; HSD_PadCopyStatus[0].button=HSD_PAD_B; squad_b_track(); squad_buttons(0);
      assert(HSD_PadCopyStatus[0].button==HSD_PAD_B && !squad_b_owned[0] && backs==before+2);
      HSD_PadCopyStatus[0].trigger=0; HSD_PadCopyStatus[0].button=0; squad_b_track(); squad_buttons(0);
      /* A on the random area with your coin in hand: a random fighter; elsewhere A is the game's drop. */
      cursors[0].x5=1; cursors[0].x6=0; fixture_coin_in_hand=true;
      HSD_PadCopyStatus[0].trigger=HSD_PAD_A; squad_buttons(0);
      assert(HSD_PadCopyStatus[0].trigger==HSD_PAD_A && ui_randoms==randoms);
      random_area=1; squad_buttons(0);
      assert(!HSD_PadCopyStatus[0].trigger && ui_randoms==randoms+1);
      fixture_coin_in_hand=false; random_area=0; cursors[0].x5=2;
      fixture_coin_pick = 7; fixture_fill_mode = false; add(0,0,9,0);
      HSD_PadCopyStatus[0].trigger=HSD_PAD_R; squad_buttons(0); /* offline VS: never a regrab */
      assert(regrabs==1 && gmSquad_Count(0)==0); }
    fixture_coin_pick = GMSQUAD_NONE; gmSquad_ClearAll();
    for (p=0;p<4;++p) {
        squad_seen_icon[p]=3; squad_icons_shown[p]=42; squad_shown[p]=43;
        squad_l_held[p]=89; squad_b_held[p]=89; squad_b_owned[p]=true; add(p,0,1,0);
    }
    mnCharSel_SquadJoin();
    assert(!gmSquad_Any());
    for(p=0;p<4;++p) assert(squad_seen_icon[p]==255 && !squad_icons_shown[p] &&
        !squad_shown[p] && !squad_l_held[p] && !squad_b_held[p] && !squad_b_owned[p]);
    puts("squad ordering, stocks, warming and held swaps: PASS");
#if MU_SQUAD_ONLINE_FIXTURE
    online_admission_test();
    online_agent_lineup_test();
    agent_css_test();
    online_setup_test();
    online_costume_test();
    friends_fixed_budget_test();
    worst_4v4_fits_test();
    console_costume_rule_test();
#endif
    return 0;
}
