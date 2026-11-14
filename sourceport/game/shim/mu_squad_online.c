/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026).
 * Squad Strike idea: Chris's friend Tyler (Squad Strike Gecko codes).
 */
#include "mu_squad.h"
#if MU_SQUAD
#ifndef MU_NO_SLIPPI
#include <string.h>
int snprintf(char* buffer, __SIZE_TYPE__ size, const char* format, ...);
#include <dolphin/dvd.h>
#include <dolphin/os.h>
#include <melee/gm/gm_1601.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/types.h>
#include <melee/lb/lbfile.h>
#include <melee/ef/types.h>
#include <mu_native.h>
#include <mu_lobby_wire.h>

static bool online_chosen, online_ready, host_supported, selection_accepted;
/* Squad Strike in a friend room (Direct): each lock-in carries its lineup, there is no search. */
static bool online_friend;
static bool lobby_chosen;
bool gmSquad_LobbyChosen(void) { return lobby_chosen; }
void gmSquad_LobbyChoose(bool chosen)
{
    if (lobby_chosen != chosen) { gmSquad_ClearAll(); gmSquad_Disable(); online_ready = false; }
    lobby_chosen = chosen;
}
bool gmSquad_LobbyAdmission(void)
{
    u8 op = MU_LOBBY_OP_SQUAD_ADMISSION;
    if (!gmSquad_OnlineBudget()) return false;
    return mu_lobby_send(&op, 1) != 0;
}
/* A friend room's lock-in, held until the friend's game is heard from (MU_SQUAD_ROOM_WAITING).
 * It is the one line shown without Squad Strike chosen: the friend may still pick it. */
static const char wait_text[] = "Waiting for your friend's game...";
/* This game can't load Squad Strike in a friend room: what it says, and the friend was told. */
static const char friend_refused[] = "Your game can't load Squad Strike lineups. Match not started.";
static bool refusal_sent;
static bool budget_scanned, budget_accepted;
static const char* online_error = "";
const char* gmSquad_OnlineError(void) { return online_chosen || online_error == wait_text ? online_error : ""; }
/* Fewer than four fighters at START or lock-in: one visible line for either path, and a log
 * line. The line goes away as soon as the lineup's count changes. */
static char need_text[48];
static int need_count = -1;
/* Set while online_error says why search cannot work (host support, memory, disc identity). */
static bool search_refused;
void gmSquad_OnlineNeedLineup(int count)
{
    if (count < 0) count = 0;
    snprintf(need_text, sizeof need_text, "Pick %d fighters first (%d/%d).", GMSQUAD_SIZE, count, GMSQUAD_SIZE);
    /* A refusal that says why search cannot work (no host support, memory) stays on screen. */
    if (!search_refused) {
        online_error = need_text;
        need_count = count;
    }
    OSReport("squad: lock-in refused, %d/%d fighters picked\n", count, GMSQUAD_SIZE);
}
void gmSquad_OnlinePickCount(int count)
{
    if (online_error == need_text && count != need_count) {
        online_error = "";
        need_count = -1;
    }
}
/* Set by setup when the exchanged squads had to be trimmed to fit memory; the splash shows it. */
static char trim_text[80];
const char* gmSquad_OnlineTrimNotice(void) { return trim_text; }
bool gmSquad_OnlineSearchReady(void)
{
    return host_supported && selection_accepted && budget_accepted;
}
static GmSquadMember own_extras[GMSQUAD_EXTRA];
static u8 own_coin_pos;
static bool saved_own;

bool gmSquad_OnlineChosen(void) { return online_chosen; }
bool gmSquad_OnlineReady(void) { return (online_chosen || lobby_chosen) && online_ready; }
bool gmSquad_OnlineFriend(void) { return online_chosen && online_friend; }
bool gmSquad_OnlineLive(int mode)
{
    return online_chosen && (online_friend ? mode == MU_SLP_MODE_DIRECT : mode == MU_SLP_MODE_UNRANKED);
}
bool gmSquad_OnlineEditable(void)
{
    const MuMatchState* match = mu_slippi_match_state();
    if (!gmSquad_OnlineLive(mu_slippi_state()->mode)) return false;
    /* A friend room meets before picks and each lock-in carries the lineup: editable until this
     * side locks in, while the friend is still on the way too. */
    if (online_friend)
        return !match->local_ready && match->connection_state != MU_SLP_MM_ERROR;
    return match->connection_state == MU_SLP_MM_IDLE ||
        (match->connection_state == MU_SLP_MM_CONNECTION_SUCCESS && !match->local_ready);
}
void gmSquad_OnlineChoose(bool chosen)
{
    u8 payload[2] = { 0, chosen ? 1 : 0 }, accepted = 0;
    unsigned got = 0;
    online_friend = false; /* the friend-room choice is made again by gmSquad_OnlineChooseFriend */
    if (online_error == wait_text) online_error = "";
    if (chosen == online_chosen) return;
    refusal_sent = false;
    gmSquad_ClearAll();
    gmSquad_Disable();
    online_chosen = chosen;
    online_ready = saved_own = selection_accepted = false;
    online_error = ""; search_refused = false;
    host_supported = !mu_slippi_cmd(MU_SQUAD_COMMAND, payload, sizeof payload, &accepted, 1, &got) &&
        got == 1 && accepted == 1;
}
void gmSquad_OnlineChooseFriend(bool friends_pick)
{
    gmSquad_OnlineChoose(true);
    online_friend = true;
    OSReport("squad: friend room squad mode (%s)\n",
             friends_pick ? "friend's pick" : "picked on the Squad Strike row");
}
/* The host's answer for this friend room (MU_SQUAD_ROOM_*), or -1 when it gave none. */
static int friend_room_answer(void)
{
    u8 query[2] = { MU_SQUAD_OP_FRIEND, MU_SQUAD_FRIEND_QUERY }, answer = 0;
    unsigned got = 0;
    if (mu_slippi_cmd(MU_SQUAD_COMMAND, query, sizeof query, &answer, 1, &got) || got != 1) return -1;
    return answer;
}
/* Squad Strike in a friend room that this game can't load: the friend's game is told once (it
 * cancels the room and says so), so nobody waits on a lineup that never comes. */
static void friend_refusal(void)
{
    u8 refuse[2] = { MU_SQUAD_OP_FRIEND, MU_SQUAD_FRIEND_REFUSE }, told = 0;
    unsigned got = 0;
    if (!online_chosen || !online_friend || !search_refused || refusal_sent) return;
    refusal_sent = !mu_slippi_cmd(MU_SQUAD_COMMAND, refuse, sizeof refuse, &told, 1, &got) && got == 1 && told == 1;
    if (refusal_sent) OSReport("squad: this game can't load Squad Strike, friend told\n");
}
void gmSquad_OnlineRoom(int mode)
{
    if (!online_chosen) return;
    /* A room joined from this screen or the results (an invite) keeps no earlier choice: Squad
     * Strike only where its own rules run, and in a friend room only when the room carries it. */
    if (!gmSquad_OnlineLive(mode)) {
        OSReport("squad: Squad Strike off, not this character select's mode\n");
        gmSquad_OnlineChoose(false);
    } else if (online_friend && friend_room_answer() != MU_SQUAD_ROOM_SQUAD) {
        OSReport("squad: Squad Strike off, this friend room is not Squad Strike\n");
        gmSquad_OnlineChoose(false);
    }
}
bool gmSquad_OnlineFriendJoin(void)
{
    const MuMatchState* match = mu_slippi_match_state();
    int answer;
    if (mu_slippi_state()->mode != MU_SLP_MODE_DIRECT || match->connection_state != MU_SLP_MM_CONNECTION_SUCCESS) {
        if (online_error == wait_text) online_error = "";
        return false;
    }
    friend_refusal();
    /* Only a connected room can have the friend's pick; never after this side locked in. */
    if (online_chosen || match->local_ready) return false;
    answer = friend_room_answer();
    if (answer != MU_SQUAD_ROOM_WAITING && online_error == wait_text) online_error = "";
    if (answer != MU_SQUAD_ROOM_SQUAD) return false;
    gmSquad_OnlineChooseFriend(true);
    gmSquad_OnlineCss();
    friend_refusal();
    return true;
}
/* gmSquad_OnlineFriendLockIn said no only to wait for the friend's game (not a refusal). */
bool gmSquad_OnlineFriendHeld(void) { return online_error == wait_text; }
bool gmSquad_OnlineFriendLockIn(void)
{
    const int answer = friend_room_answer();
    if (answer == MU_SQUAD_ROOM_WAITING) {
        if (online_error != wait_text) OSReport("squad: lock-in held, waiting for your friend's game\n");
        online_error = wait_text;
        return false;
    }
    if (online_error == wait_text) online_error = "";
    if (answer == MU_SQUAD_ROOM_SQUAD && !online_friend) {
        /* The friend picked Squad Strike just now: squads on, and this lock-in waits for four. */
        gmSquad_OnlineChooseFriend(true);
        gmSquad_OnlineCss();
        friend_refusal();
        OSReport("squad: lock-in refused, your friend picked Squad Strike\n");
        return false;
    }
    if (answer != MU_SQUAD_ROOM_SQUAD && online_friend) {
        OSReport("squad: lock-in refused, this friend room is not Squad Strike\n");
        gmSquad_OnlineChoose(false);
        return false;
    }
    return true;
}
/* The host's agent-test lineup (squadLineup), asked for each time the online squad CSS opens.
 * Only agent-test hook hosts answer; every other host sends nothing back, and the CSS stays the
 * player's. Character select takes it once (gmSquad_OnlineAgentLineup) and picks it itself. */
static MuSquadSide agent_lineup;
static bool agent_lineup_pending;
static void agent_lineup_query(void)
{
    u8 query = MU_SQUAD_OP_AGENT_LINEUP, bytes[2 * MU_SQUAD_LINEUP];
    unsigned got = 0;
    int i;
    agent_lineup_pending = false;
    if (mu_slippi_cmd(MU_SQUAD_COMMAND, &query, 1, bytes, sizeof bytes, &got) || got == 0) return;
    memset(&agent_lineup, 0, sizeof agent_lineup);
    agent_lineup.count = got == sizeof bytes ? MU_SQUAD_LINEUP : 0;
    for (i = 0; i < agent_lineup.count; ++i) {
        agent_lineup.members[i].kind = bytes[2*i];
        agent_lineup.members[i].color = bytes[2*i + 1];
    }
    if (!mu_squad_side_valid(&agent_lineup)) {
        OSReport("squad: agent lineup invalid, ignored\n");
        return;
    }
    agent_lineup_pending = true;
    {
        char text[64];
        int used = snprintf(text, sizeof text, "squad: agent lineup");
        for (i = 0; i < agent_lineup.count && used > 0 && used < (int) sizeof text; ++i)
            used += snprintf(text + used, sizeof text - (size_t) used, "%s%u:%u", i ? "," : " ",
                             agent_lineup.members[i].kind, agent_lineup.members[i].color);
        OSReport("%s\n", text);
    }
}
bool gmSquad_OnlineAgentLineup(MuSquadSide* out)
{
    if (!agent_lineup_pending || !out || !gmSquad_OnlineEditable()) return false;
    agent_lineup_pending = false;
    *out = agent_lineup;
    return true;
}
void gmSquad_OnlineCss(void)
{
    gmSquad_ClearAll();
    gmSquad_Disable();
    online_ready = selection_accepted = false;
    gmSquad_OnlineBudget(); /* first CSS entry, before lock-in; once per session */
    if (saved_own) {
        memcpy(gmSquad_rows[0], own_extras, sizeof own_extras);
        gmSquad_coin_pos[0] = own_coin_pos;
    }
    agent_lineup_query();
}
bool gmSquad_OnlineSelect(u8 coin_ck, u8 coin_color)
{
    GmSquadMember lineup[GMSQUAD_SIZE];
    u8 payload[2 + 2 * GMSQUAD_SIZE] = { 1, 0 };
    u8 accepted = 0;
    unsigned got = 0;
    int i, count;
    /* The coin's colour as the banner and the members show it: a costume past the fighter's own
     * (an m-ex 7th) plays as its last colour, so the lock-in is never refused for it. */
    coin_color = mu_squad_color(coin_ck, coin_color);
    count = gmSquad_Lineup(0, coin_ck, coin_color, lineup);
    selection_accepted = false;
    if (!online_chosen) return false;
    if (count != GMSQUAD_SIZE) {
        /* Squad Strike is exactly four fighters: refuse lock-in and say so. */
        payload[1] = 0; // still clear the host's old lineup
        mu_slippi_cmd(MU_SQUAD_COMMAND, payload, 2, &accepted, 1, &got);
        gmSquad_OnlineNeedLineup(count);
        return false;
    }
    payload[1] = (u8) count;
    for (i = 0; i < count; ++i) {
        payload[2 + 2*i] = lineup[i].ckind;
        payload[3 + 2*i] = lineup[i].color;
    }
    if (mu_slippi_cmd(MU_SQUAD_COMMAND, payload, (unsigned) (2 + 2 * count), &accepted, 1, &got) ||
        got != 1 || accepted != 1 || count == 0) {
        online_error = "Squad lineup rejected. Pick a valid fighter and try again.";
        OSReport("squad: lineup rejected, lock-in refused\n");
        return false;
    }
    memcpy(own_extras, gmSquad_rows[0], sizeof own_extras);
    own_coin_pos = gmSquad_coin_pos[0];
    saved_own = selection_accepted = true;
    online_error = ""; search_refused = false;
    {
        char text[64];
        int used = snprintf(text, sizeof text, "squad: locked in %d/%d", count, GMSQUAD_SIZE);
        for (i = 0; i < count && used > 0 && used < (int) sizeof text; ++i)
            used += snprintf(text + used, sizeof text - (size_t) used, "%s%u:%u", i ? "," : " ",
                             lineup[i].ckind, lineup[i].color);
        OSReport("%s\n", text);
    }
    return true;
}

/* Admission hashes only fixed conservative floors, the session disc table and the costume
 * constants. Costumes and colour hats are cosmetics: their facts are the fighter's constant
 * (gmSquad_CostumeBytes), never the local file, so skin packs never split the pool.
 * Local arenas can refuse the row, but never create a platform-specific pool.
 * Disc facts and host identity are cached for the session at first CSS entry. */
static bool budget_ok;
static void put32(u8* p, u32 value)
{
    p[0] = (u8) (value >> 24); p[1] = (u8) (value >> 16);
    p[2] = (u8) (value >> 8); p[3] = (u8) value;
}
static void budget_fact(void* ctx, const char* name, int use)
{
    u8 payload[137] = { 3 };
    int entry;
    size_t length;
    u32 bytes = 0xFFFFFFFFu;
    if (name == NULL) return;
    length = strlen(name);
    if (length > 126) { budget_ok = false; return; }
    if (use == GMSQUAD_FILE_COSTUME || use == GMSQUAD_FILE_HAT) {
        if (ctx == NULL) { budget_ok = false; return; }
        entry = *(const s32*) ctx; /* the fighter kind: no entry number, no local length */
        bytes = gmSquad_CostumeBytes(entry, use);
    } else bytes = gmSquad_FileSize(name, &entry);
    payload[1] = (u8) use;
    put32(payload + 2, (u32) entry);
    put32(payload + 6, bytes);
    memcpy(payload + 10, name, length + 1); /* delimited, even for absent archives */
    if (mu_slippi_cmd(MU_SQUAD_COMMAND, payload, (unsigned) (11 + length), NULL, 0, NULL))
        budget_ok = false;
}
/* The host answers the budget with 1, or 2 when a costume's standard copy (the disc's own) is
 * itself over its fighter's fixed size, 3 when that copy is a mod profile's: a skin over it loads
 * that copy instead, so only then can the trim's sizes not hold. Cosmetics never refuse a search
 * otherwise. */
static bool budget_answer(u8 accepted)
{
    if (accepted == 2 || accepted == 3) {
        online_error = accepted == 3 ?
            "This mod's costumes are too big for Squad Strike online. Search refused." :
            "This disc's own costumes are too big for Squad Strike online. Search refused.";
        search_refused = true;
        return false;
    }
    return accepted == 1;
}
extern const char* mu_ground_stage_file(int grkind);
extern int Stage_8022519C(int stkind);
bool gmSquad_OnlineBudget(void)
{
    u8 payload[9] = { 2 }, finish = 4, accepted = 0;
    u32 main_bytes, animation_bytes;
    int ck, color, target, i;
    unsigned got = 0;
    if (!online_chosen && !mu_lobby_css()) return false;
    /* Every check below sets its own refusal again: drop a stale one before re-checking. */
    if (search_refused) online_error = "";
    search_refused = false;
    if (!host_supported && !mu_lobby_css()) {
        online_error = online_friend ? friend_refused : "Host has no Squad Strike support. Search refused.";
        search_refused = true;
        OSReport("squad: host has no squad support, search refused\n");
        return false;
    }
    gmSquad_GetBudgets(&main_bytes, &animation_bytes);
    if (main_bytes < MU_SQUAD_ONLINE_ALLM || animation_bytes < MU_SQUAD_ONLINE_ALLA) {
        online_error = online_friend ? friend_refused : "This platform cannot meet Squad Strike memory budgets.";
        search_refused = true;
        OSReport("squad: platform below online budget, search refused (%u/%u)\n", main_bytes, animation_bytes);
        return false;
    }
    if (budget_scanned) {
        budget_accepted = !mu_slippi_cmd(MU_SQUAD_COMMAND, &finish, 1, &accepted, 1, &got) &&
            got == 1 && budget_answer(accepted);
        if (!budget_accepted && !search_refused) {
            online_error = online_friend ? friend_refused
                                         : "Squad Strike budget acknowledgement unavailable. Search refused.";
            search_refused = true;
            OSReport("squad: budget identity unavailable, search refused\n");
        }
        return budget_accepted;
    }
    put32(payload + 1, MU_SQUAD_ONLINE_ALLM);
    put32(payload + 5, MU_SQUAD_ONLINE_ALLA);
    budget_ok = mu_slippi_cmd(MU_SQUAD_COMMAND, payload, sizeof payload, NULL, 0, NULL) == 0;
    for (ck = 0; ck < 26; ++ck) {
        for (color = 0; color < (int) mu_squad_colors((unsigned) ck); ++color) {
            s32 extra, kind = mu_squad_fighter_kinds(ck, &extra);
            mu_squad_fighter_files(kind, (u8) color, &kind, budget_fact);
            mu_squad_fighter_files(extra, (u8) color, &extra, budget_fact);
        }
    }
    for (color = 0; color < (int) mu_squad_colors(CKind_Kirby); ++color) {
        for (target = 0; target < 26; ++target) {
            s32 extra, kind = mu_squad_fighter_kinds(target, &extra);
            mu_squad_copy_files(kind, (u8) color, &kind, budget_fact);
            mu_squad_copy_files(extra, (u8) color, &extra, budget_fact);
        }
    }
    for (i = 0; i < EF_DAT_FILE_MAX; ++i) mu_squad_effect_file(i, NULL, budget_fact);
    for (i = 0; i < 0x1D; ++i)
        budget_fact(NULL, mu_ground_stage_file(Stage_8022519C(gm_801641CC((u8) i))), false);
    if (budget_ok)
        mu_slippi_cmd(MU_SQUAD_COMMAND, &finish, 1, &accepted, 1, &got);
    budget_accepted = budget_ok && got == 1 && budget_answer(accepted);
    budget_scanned = budget_accepted;
    if (!budget_accepted && !search_refused) {
        online_error = online_friend ? friend_refused
                                     : "Squad Strike disc budget identity unavailable. Search refused.";
        search_refused = true;
        OSReport("squad: budget identity unavailable, search refused\n");
    }
    return budget_accepted;
}

/* Friends' costume rule (MU_SQUAD_OP_CONSOLE): one costume or hat fact, op 3's bytes after the
 * console op, with the same fixed size and name the identity scan sends. ctx is the fighter kind. */
static void console_fact(void* ctx, const char* name, int use)
{
    u8 payload[138] = { MU_SQUAD_OP_CONSOLE, MU_SQUAD_CONSOLE_FACT };
    size_t length;
    s32 kind;
    if (name == NULL || ctx == NULL || (use != GMSQUAD_FILE_COSTUME && use != GMSQUAD_FILE_HAT)) return;
    length = strlen(name);
    if (length > 126) return;
    kind = *(const s32*) ctx;
    payload[2] = (u8) use;
    put32(payload + 3, (u32) kind);
    put32(payload + 7, gmSquad_CostumeBytes(kind, use));
    memcpy(payload + 11, name, length + 1);
    mu_slippi_cmd(MU_SQUAD_COMMAND, payload, (unsigned) (12 + length), NULL, 0, NULL);
}
u8 gmSquad_ConsoleCostumes(bool want)
{
    u8 op[2] = { MU_SQUAD_OP_CONSOLE, MU_SQUAD_CONSOLE_OFF }, answer = 0;
    unsigned got = 0;
    int ck, color, target;
    if (!want) {
        mu_slippi_cmd(MU_SQUAD_COMMAND, op, sizeof op, NULL, 0, NULL);
        return 0;
    }
    op[1] = MU_SQUAD_CONSOLE_ARM;
    if (mu_slippi_cmd(MU_SQUAD_COMMAND, op, sizeof op, &answer, 1, &got) || got != 1) return 0;
    if (answer != 0) return answer;
    /* The host has no facts: every fighter's costumes, then every Kirby hat, as the identity scan. */
    for (ck = 0; ck < 26; ++ck) {
        for (color = 0; color < (int) mu_squad_colors((unsigned) ck); ++color) {
            s32 extra, kind = mu_squad_fighter_kinds(ck, &extra);
            mu_squad_fighter_files(kind, (u8) color, &kind, console_fact);
            mu_squad_fighter_files(extra, (u8) color, &extra, console_fact);
        }
    }
    for (color = 0; color < (int) mu_squad_colors(CKind_Kirby); ++color) {
        for (target = 0; target < 26; ++target) {
            s32 extra, kind = mu_squad_fighter_kinds(target, &extra);
            mu_squad_copy_files(kind, (u8) color, &kind, console_fact);
            mu_squad_copy_files(extra, (u8) color, &extra, console_fact);
        }
    }
    got = 0;
    if (mu_slippi_cmd(MU_SQUAD_COMMAND, op, sizeof op, &answer, 1, &got) || got != 1) return 0;
    return answer;
}

/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
 * Test-only menu/CSS substitute, called solely by --online-test squad with --local-peer. */
int gmSquad_OnlineTest(const int* picks, int n)
{
    int i, count;
    MuSquadSide side = {0};
    /* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
     * Only the host's harness, from inside its launch answer, may stand in for the menus. */
    if (!mu_online_launch_query()) return -1;
    if (!picks || n < 3 || picks[0] != MU_SQUAD_LINEUP || n != 1 + 2*picks[0]) return -1;
    count = picks[0];
    side.count = (u8) count;
    for (i = 0; i < count; ++i) {
        if (picks[1 + 2*i] < 0 || picks[1 + 2*i] > 25 || picks[2 + 2*i] < 0 || picks[2 + 2*i] > 5) return -1;
        side.members[i].kind = (u8) picks[1 + 2*i];
        side.members[i].color = (u8) picks[2 + 2*i];
    }
    if (!mu_squad_side_valid(&side)) return -1;
    gmSquad_OnlineChoose(true);
    gmSquad_OnlineCss(); // real per-disc admission hash and memory floor checks
    gmSquad_coin_pos[0] = 0;
    for (i = 1; i < count; ++i) {
        gmSquad_rows[0][i-1].ckind = side.members[i].kind;
        gmSquad_rows[0][i-1].color = side.members[i].color;
    }
    return gmSquad_OnlineSelect(side.members[0].kind, side.members[0].color) &&
           gmSquad_OnlineSearchReady() ? 0 : -1;
}

void gmSquad_OnlineSetup(StartMeleeData* start)
{
    u8 query = 5, bytes[MU_SQUAD_WIRE_MAX];
    unsigned got = 0;
    MuSquadSetup setup;
    int p, i, before[2];
    online_ready = false;
    trim_text[0] = 0;
    gmSquad_ClearAll();
    if ((!online_chosen && !lobby_chosen) || mu_slippi_cmd(MU_SQUAD_COMMAND, &query, 1, bytes, sizeof bytes, &got) ||
        !mu_squad_decode(bytes, got, &setup)) return; /* agreed normal 1v1 */
    start->rules.match_kind = MatchKind_Stock;
    start->rules.is_teams = false;
    for (p = 0; p < GMSQUAD_PORTS; ++p) {
        start->players[p].slot_type = p < 2 ? Gm_PKind_Human : Gm_PKind_NA;
        if (p >= 2) continue;
        start->players[p].ckind = (s8) setup.sides[p].members[0].kind;
        start->players[p].color = setup.sides[p].members[0].color;
        gmSquad_coin_pos[p] = 0;
        for (i = 1; i < setup.sides[p].count; ++i) {
            gmSquad_rows[p][i-1].ckind = setup.sides[p].members[i].kind;
            gmSquad_rows[p][i-1].color = setup.sides[p].members[i].color;
        }
    }
    /* Trim BEFORE the splash preloader can request the combined eight fighters.
     * It is milestone 1's exact algorithm, with canonical exchanged rows and
     * identical DVD/budget facts admitted by the compatibility key. */
    for (p = 0; p < 2; ++p) before[p] = gmSquad_Count(p);
    gmSquad_TrimOnline(start);
    if (gmSquad_Count(0) != before[0] || gmSquad_Count(1) != before[1]) {
        /* Both games trim the same exchanged rows, so both players read the same line. */
        const int a = gmSquad_Count(0) + 1, b = gmSquad_Count(1) + 1;
        if (a == b)
            snprintf(trim_text, sizeof trim_text,
                     "Squads too big to load together: each side plays %d fighter%s.", a, a == 1 ? "" : "s");
        else
            snprintf(trim_text, sizeof trim_text,
                     "Squads too big to load together: P1 plays %d, P2 plays %d.", a, b);
        OSReport("squad: %s\n", trim_text);
    }
    for (p = 0; p < 2; ++p) start->players[p].stocks = (s8) gmSquad_StocksFor();
    online_ready = true;
}
#else
/* Offline/Friends engine remains usable; this build has no squad online row. */
bool gmSquad_LobbyChosen(void) { return false; }
void gmSquad_LobbyChoose(bool chosen) { (void) chosen; }
bool gmSquad_LobbyAdmission(void) { return false; }
bool gmSquad_OnlineChosen(void) { return false; }
bool gmSquad_OnlineReady(void) { return false; }
bool gmSquad_OnlineEditable(void) { return false; }
void gmSquad_OnlineChooseFriend(bool friends_pick) { (void) friends_pick; }
bool gmSquad_OnlineFriend(void) { return false; }
bool gmSquad_OnlineLive(int mode) { (void) mode; return false; }
bool gmSquad_OnlineFriendJoin(void) { return false; }
void gmSquad_OnlineRoom(int mode) { (void) mode; }
bool gmSquad_OnlineFriendLockIn(void) { return true; }
bool gmSquad_OnlineFriendHeld(void) { return false; }
const char* gmSquad_OnlineError(void) { return ""; }
void gmSquad_OnlineNeedLineup(int count) { (void) count; }
void gmSquad_OnlinePickCount(int count) { (void) count; }
const char* gmSquad_OnlineTrimNotice(void) { return ""; }
void gmSquad_OnlineChoose(bool chosen) { (void) chosen; }
void gmSquad_OnlineCss(void) {}
bool gmSquad_OnlineSelect(u8 ck, u8 color) { (void) ck; (void) color; return false; }
bool gmSquad_OnlineAgentLineup(MuSquadSide* out) { (void) out; return false; }
bool gmSquad_OnlineBudget(void) { return false; }
u8 gmSquad_ConsoleCostumes(bool want) { (void) want; return 0; }
bool gmSquad_OnlineSearchReady(void) { return false; }
void gmSquad_OnlineSetup(StartMeleeData* start) { (void) start; }
int gmSquad_OnlineTest(const int* picks, int n) { (void) picks; (void) n; return -1; }
#endif /* MU_NO_SLIPPI */
#endif /* MU_SQUAD */
