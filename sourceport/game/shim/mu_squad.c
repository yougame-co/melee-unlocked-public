/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026).
 * Squad Strike idea: Chris's friend Tyler (Squad Strike Gecko codes).
 */
#include "mu_squad.h"
#include "mu_native.h"

#if MU_SQUAD

/* Game code uses the MSL headers; bounded host formatting is declared here. */
int snprintf(char* buffer, __SIZE_TYPE__ size, const char* format, ...);
#include <string.h>

#include <melee/gm/gm_1601.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gmvs.h>
#include <melee/gr/stage.h>
#include <melee/gm/types.h>
#include <melee/gm/gmmain_lib.h>
#include <dolphin/os.h>
#include <sysdolphin/baselib/random.h>
#include <melee/ef/efasync.h>
#include <melee/ef/types.h>
#include <melee/lb/lbfile.h>
#include <dolphin/dvd.h>
#include <melee/ft/ft_0D4D.h>
#include <melee/ft/ftdata.h>
#include <melee/ft/fighter.h>
#include <melee/ft/ftlib.h>
#include <melee/ft/types.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/lb/types.h>
#include <melee/mn/types.h>
#include <melee/pl/player.h>
#include <melee/ft/kinds/ftKirby/ftKb_Init.static.h>
#include "mu_test_env.h"

/* Squad members after the coin pick, per port (character, colour). */
GmSquadMember gmSquad_rows[GMSQUAD_PORTS][GMSQUAD_EXTRA];
u8 gmSquad_coin_pos[GMSQUAD_PORTS];

/* Ordinary game-image state, included in rollback snapshots. */
static bool s_init;

/* The match in progress is a squad match. */
static bool s_live;
/* Respawns held by gmSquad_HoldRespawn, waiting for gmSquad_OnVsFrame. */
static bool s_held[GMSQUAD_PORTS];
/* Set while gmSquad_OnVsFrame calls fn_8016719C itself. */
static bool s_respawning;
/* Stocks everyone started this squad match with: the biggest squad's size,
 * one per fighter (a smaller squad's last fighter keeps the spare ones). */
static s8 s_stocks;
/* Each player's lineup in fighting order (the order it was picked). */
static GmSquadMember s_lineup[GMSQUAD_PORTS][GMSQUAD_SIZE];
static u8 s_lineup_count[GMSQUAD_PORTS];
static u8 s_current[GMSQUAD_PORTS];
static u32 s_held_frame[GMSQUAD_PORTS];
/* Friends' costume rule (squad_console_rule): last asked armed, and a standard copy over its size
 * in this console session (never on the vanilla disc), which refuses the session's squads. */
static bool s_console_rule, s_console_over;

static void init(void)
{
    if (!s_init) {
        memset(gmSquad_rows, GMSQUAD_NONE, sizeof(gmSquad_rows));
        memset(gmSquad_coin_pos, GMSQUAD_NONE, sizeof(gmSquad_coin_pos));
        s_init = true;
    }
}

bool gmSquad_ModeAllowed(void)
{
    /* The explicit squad row runs unranked singles on each player's own CSS. */
    if (gm_GetCurrentGameMode() == GM_HANYU_CSS && (gmSquad_OnlineChosen() || gmSquad_LobbyChosen())) return true;
    /* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
     * The menu-free online harness enters debug VS after canonical setup. */
    if (gm_GetCurrentGameMode() == GM_DEBUG_VS && mu_online_harness() && gmSquad_OnlineReady()) return true;
    /* Friends runs the ordinary VS major with synced pads. */
    /* The CSS checks its live teams toggle; OnEnterVs checks the copied
     * match rules. The persistent VS copy is stale while the CSS is open. */
    return gm_GetCurrentGameMode() == GM_VS &&
           gmMainLib_GetGameRules()->mode == Mode_Stock;
}

bool gmSquad_Live(void) {
    return s_live && (gm_GetCurrentGameMode() == GM_VS ||
        ((gm_GetCurrentGameMode() == GM_HANYU_CSS ||
          (gm_GetCurrentGameMode() == GM_DEBUG_VS && mu_online_harness())) && gmSquad_OnlineReady()));
}

u32 gmSquad_Frame(void)
{
    /* Friends' engine body clock, snapshotted with the RNG, including CSS.
     * Unlike VI/retrace/time, this counts simulation bodies on both peers. */
    return mu_gm_engine_state()->unk_0;
}

u32 gmSquad_Random(int port, int salt)
{
    /* Same stream as mnCharSel_8025FB50's random coin. Friends restores
     * the canonical boot state, advances both peers to K, then checks the
     * complete state hash before releasing input (source_host.cpp).
     * Do not mix in an independently chosen clock. */
    (void) port;
    (void) salt;
    return (u32) HSD_Randi(0x7FFF);
}

/* Reservations are set once from the actual boot arenas. Ordinary game-image
 * globals, never allocator free space or host state, determine admission. */
static u32 s_allm_budget;
static u32 s_alla_budget;

/* The immutable disc table is opened at most once per entry per session online.
 * Repeated fighter/costume/Kirby references reuse it both for identity and trim. */
static struct { int entry; u32 bytes; } s_disc_sizes[2048];
static unsigned s_disc_count;
u32 gmSquad_FileSize(const char* name, int* entry)
{
    DVDFileInfo info;
    u32 bytes = 0xFFFFFFFFu;
    unsigned i;
    *entry = DVDConvertPathToEntrynum(lbFileGetFullName(name));
    if (gmSquad_OnlineChosen() || mu_lobby_css()) {
        for (i = 0; i < s_disc_count; ++i)
            if (s_disc_sizes[i].entry == *entry) return s_disc_sizes[i].bytes;
        if (s_disc_count == 2048) return bytes; // fail closed
    }
    if (*entry >= 0 && DVDFastOpen(*entry, &info)) {
        bytes = info.length;
        DVDClose(&info);
    }
    if (gmSquad_OnlineChosen() || mu_lobby_css()) {
        s_disc_sizes[s_disc_count].entry = *entry;
        s_disc_sizes[s_disc_count++].bytes = bytes;
    }
    return bytes;
}

/* Costumes are cosmetics: a skin may replace any of them, so online they are never read from the
 * disc. Each is charged at its fighter's largest costume on the vanilla NTSC 1.02 disc (GALE01
 * rev 2 file table, the largest of that fighter's colour slots), and each Kirby colour hat at the
 * largest of that copy's colour hats; Captain Falcon's is the English game's PlCaRe.usd. Indexed by
 * fighter kind (Ft_Kind_Mario 0 .. Sandbag 32). Every player charges the same bytes, so the budget
 * identity and the trim are the same for any skin pack; in an online squad match a local file over
 * its size loads the disc's own copy instead (the host's file lookup, squad_costume_rule.h).
 * Changing a value changes the identity: the game hashes the table. */
static const u32 s_costume_worst[33] = {
    473526, 362982, 558539, 641744, 283994, 518222, 491576, 380180, /* Mr Fx Ca Dk Kb Kp Lk Sk */
    375368, 809794, 350744, 350748, 166982, 407885, 295258, 264147, /* Ns Pe Pp Nn Pk Ss Ys Pr */
    263344, 487174, 594964, 350082, 452050, 483018, 243998, 316154, /* Mt Lg Ms Zd Cl Dr Fc Pc */
    113895, 391121, 660288, 313338, 313513,  79135,  84444, 515320, /* Gw Gn Fe Mh Ch Bo Gl Gk */
     47106,                                                         /* Sb */
};
static const u32 s_hat_worst[33] = {
    [3] = 181734, [15] = 185398, [16] = 171230, [22] = 186678, [24] = 108938, /* Dk Pr Mt Fc Gw */
};
u32 gmSquad_CostumeBytes(int kind, int use)
{
    if (kind < 0 || kind >= 33) return 0;
    return use == GMSQUAD_FILE_HAT ? s_hat_worst[kind] : use == GMSQUAD_FILE_COSTUME ? s_costume_worst[kind] : 0;
}

void gmSquad_GetBudgets(u32* main_bytes, u32* animation_bytes)
{
    *main_bytes = s_allm_budget;
    *animation_bytes = s_alla_budget;
}

u32 mu_squad_main_heap(u32 original, uintptr_t lo, uintptr_t hi)
{
    u32 base_main, floor, grown;
    s_allm_budget = original;
    if (hi <= lo || hi - lo <= original || original > 0x7FFFFFFFu)
        return original;
    base_main = (u32) (hi - lo) - original;
    /* Two active fighters need the main heap even with eight archives loaded.
     * Keep at least 70% of the base main heap and an absolute 18 MiB floor.
     * Retail and mod-asset 40 MiB layouts both allow 2x; never blindly use 3x. */
    floor = base_main / 10u * 7u;
    if (floor < 18u * 1024u * 1024u) floor = 18u * 1024u * 1024u;
    grown = original * 2u;
    if (hi - lo < (uintptr_t) grown + floor) grown = original;
    s_allm_budget = grown;
    return grown;
}

u32 mu_squad_animation_heap(u32 original, uintptr_t lo, uintptr_t hi)
{
    u32 available = hi > lo ? (u32) (hi - lo) & ~31u : 0;
    /* Heap 1's retail match user is ftData_80085A14's animation fallback.
     * With every fighter preloaded it needs zero bytes; stages, effects and
     * items use MEM1. Keep 1 MiB for fallback/fragmentation, also the maker's
     * documented ARAM_KEEP floor (donor_load.c). Refuse oversized lineups. */
    (void) original;
    s_alla_budget = available > 0x100000u ? available - 0x100000u : 0;
    return s_alla_budget;
}

u32 mu_squad_sound_heap(u32 original, const u32 banks[8])
{
    int i;
    /* Original already has one group-3 bank, four group-4 fighter banks and
     * one group-5 stage bank. Only add fighter banks 5..8; do not double the
     * fixed group-3/group-5 reservations. The caller sorts the real table. */
    for (i = 4; i < 8; ++i) original += banks[i];
    return original;
}

int gmSquad_StockRule(bool active, int current, int need,
                      int* saved, int* applied)
{
    /* An external Rules edit becomes the user's setting, even on the body
     * that switches out of Stock. CSS exit restores before opening Rules,
     * so selecting a value equal to the squad count also works. */
    if (*saved >= 0 && *applied >= 0 && current != *applied) *saved = current;
    if (active) {
        if (*saved < 0) *saved = current;
        *applied = need;
        return need;
    }
    if (*saved >= 0) current = *saved;
    *saved = *applied = -1;
    return current;
}

int gmSquad_Count(int port)
{
    int n = 0;
    int i;
    init();
    if (port < 0 || port >= GMSQUAD_PORTS) {
        return 0;
    }
    for (i = 0; i < GMSQUAD_EXTRA; i++) {
        if (gmSquad_rows[port][i].ckind != GMSQUAD_NONE) {
            n++;
        }
    }
    return n;
}

int gmSquad_StocksFor(void)
{
    int need = 0;
    int p;
    for (p = 0; p < GMSQUAD_PORTS; p++) {
        if (gmSquad_Count(p) + 1 > need) {
            need = gmSquad_Count(p) + 1;
        }
    }
    return need * GMSQUAD_STOCKS_PER_FIGHTER;
}

bool gmSquad_Any(void)
{
    int p;
    for (p = 0; p < GMSQUAD_PORTS; p++) {
        if (gmSquad_Count(p) != 0) {
            return true;
        }
    }
    return false;
}

void gmSquad_Clear(int port)
{
    init();
    if (port >= 0 && port < GMSQUAD_PORTS) {
        memset(gmSquad_rows[port], GMSQUAD_NONE, sizeof(gmSquad_rows[port]));
        gmSquad_coin_pos[port] = GMSQUAD_NONE;
    }
}

void gmSquad_TrackCoin(int port, bool has_coin_pick)
{
    int n;
    init();
    if (port < 0 || port >= GMSQUAD_PORTS) {
        return;
    }
    n = gmSquad_Count(port);
    if (!has_coin_pick) {
        /* Coin in hand: it is out of the order until it goes down again. */
        gmSquad_coin_pos[port] = GMSQUAD_NONE;
    } else if (gmSquad_coin_pos[port] == GMSQUAD_NONE) {
        /* The coin just went down: after everyone picked before it. */
        gmSquad_coin_pos[port] = (u8) n;
    } else if (gmSquad_coin_pos[port] > n) {
        gmSquad_coin_pos[port] = (u8) n; /* members removed (R) */
    }
}

int gmSquad_Lineup(int port, u8 coin_ck, u8 coin_color,
                   GmSquadMember out[GMSQUAD_SIZE])
{
    int n = 0, k, members = 0, at;
    init();
    if (port < 0 || port >= GMSQUAD_PORTS) {
        return 0;
    }
    at = gmSquad_coin_pos[port] == GMSQUAD_NONE ? 0 : gmSquad_coin_pos[port];
    for (k = 0; k < GMSQUAD_EXTRA; k++) {
        if (gmSquad_rows[port][k].ckind == GMSQUAD_NONE) {
            continue;
        }
        if (members == at && coin_ck != GMSQUAD_NONE) {
            out[n].ckind = coin_ck;
            out[n].color = coin_color;
            n++;
        }
        out[n++] = gmSquad_rows[port][k];
        members++;
    }
    if (members <= at && coin_ck != GMSQUAD_NONE) {
        out[n].ckind = coin_ck;
        out[n].color = coin_color;
        n++;
    }
    return n;
}

void gmSquad_ClearAll(void)
{
    init();
    memset(gmSquad_rows, GMSQUAD_NONE, sizeof(gmSquad_rows));
    memset(gmSquad_coin_pos, GMSQUAD_NONE, sizeof(gmSquad_coin_pos));
}

u32 gmSquad_Key(void)
{
    u32 key = 2166136261u;
    const u8* p = (const u8*) gmSquad_rows;
    size_t i;
    init();
    for (i = 0; i < sizeof(gmSquad_rows); i++) {
        key = (key ^ p[i]) * 16777619u;
    }
    for (i = 0; i < GMSQUAD_PORTS; i++) {
        key = (key ^ gmSquad_coin_pos[i]) * 16777619u;
    }
    return key | 1u; /* zero is the unmodified/no-squad preload key */
}

u64 gmSquad_SoundMask(void)
{
    u64 mask = 0;
    int p, i;
    init();
    if (!gmSquad_ModeAllowed()) {
        return 0;
    }
    for (p = 0; p < GMSQUAD_PORTS; p++) {
        for (i = 0; i < GMSQUAD_EXTRA; i++) {
            u8 ck = gmSquad_rows[p][i].ckind;
            if (ck < ChKind_Max) {
                mask |= lbAudioAx_80026E84(ck);
            }
        }
    }
    return mask;
}

/* A file table entry is charged once, as lbDvd_80017740 deduplicates it.
 * Include the preloader's 0x100 headroom per file (archive header/alignment).
 * No free-space queries, asynchronous preload state, native_alloc or host
 * clocks participate in this decision. */
typedef struct SquadFileBudget {
    int entries[128];
    int count;
    u32 main_bytes, animation_bytes;
    int kind; /* the fighter the next visits are for, for its costume constant */
    const char* costumes[64]; /* 4 ports: at most 24 costumes and 36 colour hats */
    int costume_count;
} SquadFileBudget;

extern EF_DAT_Entry efAsync_DatEntries[];
extern const char* mu_ground_stage_file(int grkind);

void mu_squad_effect_file(int index, void* ctx, GmSquadFileVisitor visit)
{
    if (index >= 0 && index < EF_DAT_FILE_MAX)
        visit(ctx, efAsync_DatEntries[index].ef_DAT_file, false);
}

int mu_console_active(void);
/* Decisions two machines must make alike use the fixed facts: the online row, and Friends' synced
 * select (a console session runs this offline engine on both machines, gmSquad_Frame). There each
 * machine's own heap and its own skins' lengths would let squad_add, squad_place_coin or
 * squad_trim decide differently on each side, a desync. Every native host boots with at least the
 * fixed floors (mu_squad_wire.h), so both machines can load what the floors admit. */
static bool squad_fixed_budget(void)
{
    return gmSquad_OnlineChosen() || gmSquad_LobbyChosen() || mu_console_active();
}

static void squad_budget_file(void* ctx, const char* name, int use)
{
    SquadFileBudget* b = ctx;
    int entry, i;
    u32 bytes;
    if (name == NULL) return;
    if ((use == GMSQUAD_FILE_COSTUME || use == GMSQUAD_FILE_HAT) && squad_fixed_budget()) {
        /* Online and Friends: the fighter's constant, never the local skin (the host opens the standard copy
         * for a skin over it). Offline (one machine's heap, nothing exchanged) costumes are charged
         * at their local length below, exactly as before. Same name = same archive. */
        bytes = gmSquad_CostumeBytes(b->kind, use);
        for (i = 0; i < b->costume_count; ++i) if (!strcmp(b->costumes[i], name)) return;
        if (bytes == 0 || b->costume_count == 64) {
            b->main_bytes = b->animation_bytes = 0x7FFFFFFFu;
            return; /* unknown costume is deterministically unaffordable */
        }
        b->costumes[b->costume_count++] = name;
        b->main_bytes += ((bytes + 31u) & ~31u) + 0x100u;
        return;
    }
    bytes = gmSquad_FileSize(name, &entry);
    for (i = 0; i < b->count; ++i) if (b->entries[i] == entry) return;
    if (entry < 0 || b->count == 128 || bytes == 0xFFFFFFFFu) {
        b->main_bytes = b->animation_bytes = 0x7FFFFFFFu;
        return; /* missing file is deterministically unaffordable */
    }
    b->entries[b->count++] = entry;
    bytes = (bytes + 31u) & ~31u;
    if (use == GMSQUAD_FILE_ANIMATION) b->animation_bytes += bytes + 0x100u;
    else b->main_bytes += bytes + 0x100u;
}

static u32 squad_stage_bytes(int stage)
{
    const char* name = mu_ground_stage_file(Stage_8022519C(stage));
    int entry;
    u32 bytes;
    if (name == NULL) return 0;
    bytes = gmSquad_FileSize(name, &entry);
    if (entry < 0 || bytes == 0xFFFFFFFFu) return 0x40000000u;
    bytes = ((bytes + 31u) & ~31u) + 0x100u;
    return bytes;
}

bool gmSquad_Budget(const GmSquadMember* members, int count, int stage)
{
    SquadFileBudget b;
    int i, j;
    const bool fixed = squad_fixed_budget();
    const u32 allm = fixed ? MU_SQUAD_ONLINE_ALLM : s_allm_budget;
    const u32 alla = fixed ? MU_SQUAD_ONLINE_ALLA : s_alla_budget;
    u32 stage_bytes = 0;
    if (s_console_over && mu_console_active()) {
        OSReport("squad: a costume's standard copy is over its size, Friends squads refused\n");
        return false;
    }
    /* Friends decides on the floors; this machine must also be able to load what they admit, as
     * online refuses its search (gmSquad_OnlineBudget). Every native host boots over them. */
    if (mu_console_active() && (s_allm_budget < MU_SQUAD_ONLINE_ALLM || s_alla_budget < MU_SQUAD_ONLINE_ALLA)) {
        OSReport("squad: platform below the Friends budget (%u/%u), squads refused\n",
                 s_allm_budget, s_alla_budget);
        return false;
    }
    memset(&b, 0, sizeof b);
    if (stage >= 0) stage_bytes = squad_stage_bytes(stage);
    else {
        /* CSS has no final stage yet. Reserve the largest retail VS stage
         * from the game's existing 29-stage demo list, using DVD lengths. */
        for (i = 0; i < 0x1D; ++i) {
            u32 bytes = squad_stage_bytes(gm_801641CC((u8) i));
            if (bytes > stage_bytes) stage_bytes = bytes;
        }
    }
    /* Stage plus 256 KiB for shared preload files and transition headroom. */
    b.main_bytes = stage_bytes + 0x40000u;
    for (i = 0; i < count; ++i) {
        s32 extra, kind = mu_squad_fighter_kinds(members[i].ckind, &extra);
        b.kind = kind;
        mu_squad_fighter_files(kind, members[i].color, &b, squad_budget_file);
        b.kind = extra;
        mu_squad_fighter_files(extra, members[i].color, &b, squad_budget_file);
    }
    for (j = 0; j < count; ++j) {
        if (members[j].ckind != CKind_Kirby) continue;
        for (i = 0; i < count; ++i) {
            s32 extra, kind = mu_squad_fighter_kinds(members[i].ckind, &extra);
            b.kind = kind;
            mu_squad_copy_files(kind, members[j].color, &b, squad_budget_file);
            b.kind = extra;
            mu_squad_copy_files(extra, members[j].color, &b, squad_budget_file);
        }
    }
    if (b.animation_bytes > alla) {
        OSReport("squad: lineup over animation budget (%u > %u)\n",
                 b.animation_bytes, alla);
        return false;
    }
    if (b.main_bytes > allm) {
        OSReport("squad: lineup over main budget (%u > %u)\n",
                 b.main_bytes, allm);
        return false;
    }
    return true;
}

/* Deterministic back-to-front trimming: one extra from each active port per
 * pass, ascending port order. Coin picks remain ordinary VS picks. */
static void squad_trim(StartMeleeData* start)
{
    for (;;) {
        GmSquadMember members[GMSQUAD_PORTS * GMSQUAD_SIZE];
        int count = 0, p, i;
        bool removed = false;
        for (p = 0; p < GMSQUAD_PORTS; ++p) {
            if (start->players[p].slot_type == Gm_PKind_NA) continue;
            count += gmSquad_Lineup(p, (u8) start->players[p].ckind,
                start->players[p].color, &members[count]);
        }
        if (gmSquad_Budget(members, count, start->rules.stkind)) return;
        for (p = 0; p < GMSQUAD_PORTS; ++p) {
            if (start->players[p].slot_type == Gm_PKind_NA) continue;
            for (i = GMSQUAD_EXTRA - 1; i >= 0; --i) {
                if (gmSquad_rows[p][i].ckind == GMSQUAD_NONE) continue;
                OSReport("squad: trimmed P%d %u:%u at setup\n", p + 1,
                    gmSquad_rows[p][i].ckind, gmSquad_rows[p][i].color);
                gmSquad_rows[p][i].ckind = GMSQUAD_NONE;
                gmSquad_TrackCoin(p, true);
                removed = true;
                break;
            }
        }
        if (!removed) return;
    }
}

void gmSquad_TrimOnline(StartMeleeData* start) { squad_trim(start); }

/* ---- preloading ---------------------------------------------------------- */

/* Friends' synced select charges costumes and hats at their fighter's fixed size (squad_budget_file)
 * on a main heap with no room over them, so while squads are in play in a console session the host
 * opens the disc's copy for a skin over its size, as online. Asked before every preload list, so it
 * holds before a coin's or a member's files load; the answer is the same on both machines of a
 * session (one canonical view). Off once when the session's squads or the session end. */
static void squad_console_rule(void)
{
    const bool console = mu_console_active() != 0;
    const bool want = console && gmSquad_ModeAllowed() && (gmSquad_Any() || s_live);
    u8 answer;
    if (!console) s_console_over = false;
    if (!want && !s_console_rule) return;   /* nothing armed, nothing to switch off */
    answer = gmSquad_ConsoleCostumes(want);
    s_console_rule = want;
    if (want && (answer == 2 || answer == 3)) {
        if (!s_console_over) OSReport("squad: a costume's standard copy is over its size, Friends squads refused\n");
        s_console_over = true;
    }
}

void gmSquad_AddPreloads(struct GameCache* cache)
{
    CharacterKind lineup[GMSQUAD_PORTS * GMSQUAD_SIZE];
    u8 kirby_colors[GMSQUAD_PORTS * GMSQUAD_SIZE];
    int n_lineup = 0;
    int n_kirby = 0;
    bool any_coin = false;
    int p, i, j;

    init();
    squad_console_rule();
    if (!gmSquad_ModeAllowed() || !gmSquad_Any()) {
        return;
    }
    /* Character select releases every character for a moment on entry so
     * the last match's files are freed rather than moved; adding squads in
     * that moment would keep them alive. Only add while a coin is down. */
    for (p = 0; p < GMSQUAD_PORTS; p++) {
        if (cache->entries[p].char_id != ChKind_None) {
            any_coin = true;
        }
    }
    if (!any_coin) {
        return;
    }

    for (p = 0; p < GMSQUAD_PORTS; p++) {
        CharacterKind coin = cache->entries[p].char_id;
        if (coin < ChKind_Max) {
            lineup[n_lineup++] = coin;
            if (coin == CKind_Kirby) {
                kirby_colors[n_kirby++] = cache->entries[p].color;
            }
        }
        for (i = 0; i < GMSQUAD_EXTRA; i++) {
            GmSquadMember* m = &gmSquad_rows[p][i];
            if (m->ckind >= ChKind_Max) {
                continue;
            }
            Player_80031CB0(m->ckind, m->color);
            lineup[n_lineup++] = m->ckind;
            if (m->ckind == CKind_Kirby) {
                kirby_colors[n_kirby++] = m->color;
            }
        }
    }

    /* Every Kirby can copy anyone in either lineup; the preloader ignores
     * repeated requests. */
    for (j = 0; j < n_kirby; j++) {
        for (i = 0; i < n_lineup; i++) {
            if (lineup[i] != CKind_Kirby) {
                Player_80031D2C(lineup[i], kirby_colors[j]);
            }
        }
    }
}

/* ---- match ------------------------------------------------------------- */

void gmSquad_Disable(void)
{
    s_live = false;
    memset(s_held, 0, sizeof(s_held));
    s_respawning = false;
}

/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026).
 * Includes a fix from v1.0.1; the author's confirmation for that version is pending.
 * Two players on the same fighter in the same costume can't be told apart, and
 * online each side picks without seeing the other. The later port gets the next
 * tint, the rule the game itself uses for teams. Visual only, from synced data. */
static void squad_tint_start(StartMeleeData* start)
{
    int i, j;
    for (i = 0; i < GMSQUAD_PORTS; i++) {
        start->players[i].sub_color = 0;
        if (start->players[i].slot_type == Gm_PKind_NA) {
            continue;
        }
        for (j = 0; j < i; j++) {
            if (start->players[j].slot_type != Gm_PKind_NA &&
                start->players[i].ckind == start->players[j].ckind &&
                start->players[i].color == start->players[j].color &&
                start->players[i].sub_color == start->players[j].sub_color)
            {
                start->players[i].sub_color++;
                j = -1; /* recheck against everyone with the new tint */
            }
        }
    }
}

void gmSquad_OnEnterVs(StartMeleeData* start)
{
    /* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
     * mu_entry.c's ABI wrapper exists with and without Slippi. Announce every
     * match so a squad with recording disabled cannot taint a later recording. */
    extern int mu_slippi_command(u8 command, const u8* payload, unsigned int size,
                                 u8* response, unsigned int capacity, unsigned int* got);
    u8 replay_match[2] = { MU_SQUAD_OP_REPLAY_MATCH, 0 };
    int players = 0;
    bool any = false;
    int i;

    mu_slippi_command(MU_SQUAD_COMMAND, replay_match, sizeof replay_match, NULL, 0, NULL);
    gmSquad_Disable();
    init();
    if (gm_GetCurrentGameMode() == GM_HANYU_CSS ||
        (gm_GetCurrentGameMode() == GM_DEBUG_VS && mu_online_harness())) {
        if (!gmSquad_OnlineReady()) return; /* normal fallback cannot use local picker state */
        any = true; /* a 1-member squad versus a 1-member squad is still a squad match */
    }
    if (!gmSquad_ModeAllowed() || start->rules.match_kind != MatchKind_Stock || start->rules.is_teams) {
        return;
    }
    for (i = 0; i < GM_MAX_PLAYERS; i++) {
        if (start->players[i].slot_type == Gm_PKind_NA) {
            continue;
        }
        players++;
        if (i < GMSQUAD_PORTS && gmSquad_Count(i) != 0) {
            any = true;
        }
    }
    if (!any || players != 2) {
        return;
    }
    /* No special/added fighters: only the consented retail Squad Strike
     * mode is ported. Reject the whole match before changing any player. */
    for (i = 0; i < GM_MAX_PLAYERS; i++) {
        if (start->players[i].slot_type != Gm_PKind_NA &&
            (i >= GMSQUAD_PORTS || (u8) start->players[i].ckind >= 26)) return;
    }
    squad_trim(start);
    if (!gmSquad_Any() && !gmSquad_OnlineReady()) return;
    s_live = true;
    replay_match[1] = 1;
    mu_slippi_command(MU_SQUAD_COMMAND, replay_match, sizeof replay_match, NULL, 0, NULL);
    s_stocks = 1;
    for (i = 0; i < GMSQUAD_PORTS; i++) {
        if (start->players[i].slot_type != Gm_PKind_NA &&
            gmSquad_Count(i) + 1 > s_stocks)
            s_stocks = (s8) (gmSquad_Count(i) + 1);
    }
    memset(s_lineup, GMSQUAD_NONE, sizeof(s_lineup));
    memset(s_lineup_count, 0, sizeof(s_lineup_count));
    memset(s_current, 0, sizeof(s_current));
    for (i = 0; i < GM_MAX_PLAYERS; i++) {
        int k;
        if (start->players[i].slot_type == Gm_PKind_NA) {
            continue;
        }
        start->players[i].stocks = s_stocks;
        if (i >= GMSQUAD_PORTS) {
            continue;
        }
        s_lineup_count[i] = (u8) gmSquad_Lineup(
            i, (u8) start->players[i].ckind, start->players[i].color,
            s_lineup[i]);
        /* The squad fights in the order it was picked: whoever was picked
         * first starts, coin pick or not. (start is the match's own copy;
         * character select keeps its coin pick.) */
        start->players[i].ckind = s_lineup[i][0].ckind;
        start->players[i].color = s_lineup[i][0].color;
        {
            /* Which fighters this match loads, for a log that ends in a
             * crash: character kind:colour, in lineup order. */
            char line[96];
            int n = snprintf(line, sizeof line, "squad: P%d", i + 1);
            for (k = 0; k < s_lineup_count[i] && n > 0 && n < (int) sizeof line; k++) {
                n += snprintf(line + n, sizeof line - (size_t) n, " %d:%d",
                              s_lineup[i][k].ckind, s_lineup[i][k].color);
            }
            OSReport("%s  (%d stocks)\n", line, s_stocks);
        }
    }
    squad_tint_start(start);
}

void gmSquad_ApplyLiveStart(StartMeleeData* start)
{
    int p;
    if (!gmSquad_Live()) return;
    start->rules.match_kind = MatchKind_Stock;
    start->rules.is_teams = false;
    for (p = 0; p < GMSQUAD_PORTS; ++p) {
        if (!s_lineup_count[p]) continue;
        start->players[p].ckind = s_lineup[p][0].ckind;
        start->players[p].color = s_lineup[p][0].color;
        start->players[p].stocks = s_stocks;
    }
    squad_tint_start(start);
}

bool gmSquad_StockIconFrame(int slot, int i, f32* frame)
{
    StaticPlayer* ps;
    int index;
    GmSquadMember* m;

    if (!gmSquad_Live() || slot < 0 || slot >= GMSQUAD_PORTS || i <= 0 ||
        s_lineup_count[slot] <= 1)
    {
        return false; /* the fighter out now: the game's own icon */
    }
    ps = Player_GetPtrForSlot(slot);
    /* Stock i from the left is lost after i more KOs: whoever fights then. */
    index = (s_stocks - ps->stocks) / GMSQUAD_STOCKS_PER_FIGHTER + i;
    if (index >= s_lineup_count[slot]) {
        index = s_lineup_count[slot] - 1;
    }
    if (index < 0) {
        return false;
    }
    m = &s_lineup[slot][index];
    if (m->ckind >= ChKind_Max) {
        return false;
    }
    *frame = gm_80168B34(m->ckind, 0, m->color);
    return true;
}

/* What Fighter_Create loads for this fighter, so its archives are parsed now
 * (during setup, which netplay never rolls back) instead of at the swap.
 * Re-parsing an already relocated archive after a rollback would corrupt
 * it; these loaders cache their results and do nothing the second time. */
static void warm_fighter(s32 kind, u8 color)
{
    if (kind < 0) {
        return;
    }
    ftData_8008572C(kind);
    if (ftData_UnkBytePerCharacter[kind] != 0xFF) {
        efAsync_LoadSync(ftData_UnkBytePerCharacter[kind]);
    }
    ftData_80085820(kind, color);
    ftData_80085A14(kind);
}

static void warm_copy(s32 kind, u8 kirby_color)
{
    if (kind >= 0 && kind != Ft_Kind_Kirby) {
        Player_80031DA8(kind, kirby_color);
    }
}

void gmSquad_WarmFiles(void)
{
    CharacterKind lineup[GMSQUAD_PORTS][GMSQUAD_SIZE];
    u8 colors[GMSQUAD_PORTS][GMSQUAD_SIZE];
    int count[GMSQUAD_PORTS];
    int p, i, q, k;

    if (!gmSquad_Live()) {
        return;
    }
    for (p = 0; p < GMSQUAD_PORTS; p++) {
        count[p] = 0;
        if (Player_GetPlayerSlotType(p) == Gm_PKind_NA) {
            continue;
        }
        /* Include both internal fighters of every member, even the starter. */
        for (i = 0; i < s_lineup_count[p]; i++) {
            GmSquadMember* m = &s_lineup[p][i];
            if (m->ckind < ChKind_Max) {
                s32 extra;
                s32 kind = mu_squad_fighter_kinds(m->ckind, &extra);
                warm_fighter(kind, m->color);
                warm_fighter(extra, m->color);
                lineup[p][count[p]] = m->ckind;
                colors[p][count[p]] = m->color;
                count[p]++;
            }
        }
    }
    /* Kirby copies: each Kirby (in its colour) copies anyone on the other
     * side, whoever is on stage when it swallows them. */
    for (p = 0; p < GMSQUAD_PORTS; p++) {
        for (i = 0; i < count[p]; i++) {
            if (lineup[p][i] != CKind_Kirby) {
                continue;
            }
            for (q = 0; q < GMSQUAD_PORTS; q++) {
                if (q == p) {
                    continue;
                }
                for (k = 0; k < count[q]; k++) {
                    s32 extra;
                    s32 kind = mu_squad_fighter_kinds(lineup[q][k], &extra);
                    warm_copy(kind, colors[p][i]);
                    warm_copy(extra, colors[p][i]);
                }
            }
        }
    }
}

/* The squad member due now for this player, or NULL to respawn as is. */
static GmSquadMember* due_member(s32 slot)
{
    StaticPlayer* ps;
    int lost;
    int index;
    GmSquadMember* m;

    if (!gmSquad_Live() || slot < 0 || slot >= GMSQUAD_PORTS) {
        return NULL;
    }
    ps = Player_GetPtrForSlot(slot);
    if (ps->pkind != Gm_PKind_Human && ps->pkind != Gm_PKind_Cpu) {
        return NULL;
    }
    if (ps->player_state != 2 || ps->stocks <= 0) {
        return NULL;
    }
    lost = s_stocks - ps->stocks;
    if (lost <= 0 || lost % GMSQUAD_STOCKS_PER_FIGHTER != 0) {
        return NULL;
    }
    index = lost / GMSQUAD_STOCKS_PER_FIGHTER;
    if (index <= s_current[slot] || index >= s_lineup_count[slot]) {
        return NULL;
    }
    m = &s_lineup[slot][index];
    if (m->ckind >= 26) {
        return NULL;
    }
    return m;
}

bool gmSquad_HoldRespawn(s32 slot, s32 subchar)
{
    if (s_respawning || subchar != 0 || due_member(slot) == NULL) {
        return false;
    }
    /* Don't swap here: this can run inside the dying fighter's own
     * callbacks, and the swap frees that fighter. The next frame's
     * gmSquad_OnVsFrame does it from the scene. */
    if (!s_held[slot]) s_held_frame[slot] = gmSquad_Frame();
    s_held[slot] = true;
    return true;
}

/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026).
 * Includes a fix from v1.0.1; the author's confirmation for that version is pending.
 * squad_tint_start's rule again after a swap, from who is on stage now. The fighter
 * that stays out takes its new tint too. Runs inside the simulation from synced data,
 * so a rollback repeats it the same way. */
static void squad_retint(void)
{
    s8 tint[GMSQUAD_PORTS];
    int i, j, k;

    for (i = 0; i < GMSQUAD_PORTS; i++) {
        StaticPlayer* ps = Player_GetPtrForSlot(i);
        tint[i] = 0;
        if (Player_GetPlayerSlotType(i) == Gm_PKind_NA) {
            continue;
        }
        for (j = 0; j < i; j++) {
            if (Player_GetPlayerSlotType(j) != Gm_PKind_NA &&
                Player_GetPlayerCharacter(i) == Player_GetPlayerCharacter(j) &&
                Player_GetCostumeId(i) == Player_GetCostumeId(j) && tint[i] == tint[j])
            {
                tint[i]++;
                j = -1;
            }
        }
        Player_SetSubColor(i, tint[i]);
        for (k = 0; k < 2; k++) {
            if (ps->player_entity[k] != NULL) {
                Fighter* fp = GET_FIGHTER(ps->player_entity[k]);
                fp->sub_color = (u8) tint[i];
                /* Colour overlays blend from this value, which the game only computes when a fighter is created. */
                if (tint[i] != 0) {
                    GXColor* color = &p_ftCommonData->sub_colors[tint[i] - 1];
                    fp->x610_color_rgba[0].r = (color->r * color->a) / 255;
                    fp->x610_color_rgba[0].g = (color->g * color->a) / 255;
                    fp->x610_color_rgba[0].b = (color->b * color->a) / 255;
                    fp->x610_color_rgba[0].a = color->a;
                } else {
                    fp->x610_color_rgba[0].r = fp->x610_color_rgba[0].g = fp->x610_color_rgba[0].b = 0;
                    fp->x610_color_rgba[0].a = 0;
                }
            }
        }
    }
}

static void swap_in(s32 slot, GmSquadMember* m)
{
    StaticPlayer* ps = Player_GetPtrForSlot(slot);

    Player_80031EBC(slot); /* free the fighter that's out (and Nana/Sheik) */
    ps->player_entity[0] = NULL;
    ps->player_entity[1] = NULL;
    ps->transformed[0] = 0; /* not transformed: Zelda, not Sheik */
    ps->transformed[1] = 1;
    ps->flags.b2 = false; /* "has a second fighter": the new one decides */

    Player_SetPlayerCharacter(slot, m->ckind);
    Player_SetCostumeId(slot, m->color);
    Player_80031AD0(slot); /* create the new fighter(s) */
    squad_retint();
    Player_80031848(slot);

    if (m->ckind == CKind_PopoNana) {
        /* The game only revives a partner that is asleep, and a new Nana
         * isn't: put her to sleep so she drops in beside Popo. */
        HSD_GObj* nana = ps->player_entity[ps->transformed[1]];
        if (nana != NULL) {
            ftCo_800D4F24(nana, 0);
        }
    }
}

/* Test only (MELEE_TEST_SQUAD_KIRBY=1), after a swap: whether each Kirby on stage already has the
 * copy files (hat archive and its costume's hat) of every fighter on the other side. Vanilla loads
 * them for the match's fighters at the start; gmSquad_WarmFiles loads them for every lineup member,
 * so a fighter who swaps in later should be ready without loading again after each swap. */
static void test_kirby_copies(void)
{
    static int on = -1;
    int k, v, side;

    if (on < 0) {
        const char* e = mu_test_getenv("MELEE_TEST_SQUAD_KIRBY");
        on = e != NULL && *e == '1';
    }
    if (!on) {
        return;
    }
    for (k = 0; k < GMSQUAD_PORTS; k++) {
        int color;
        if (Player_GetPlayerSlotType(k) == Gm_PKind_NA || Player_GetPlayerCharacter(k) != CKind_Kirby) {
            continue;
        }
        color = mu_mex_parts_costume(Ft_Kind_Kirby, Player_GetCostumeId(k));
        for (v = 0; v < GMSQUAD_PORTS; v++) {
            if (v == k || Player_GetPlayerSlotType(v) == Gm_PKind_NA) {
                continue;
            }
            for (side = 0; side < 2; side++) {
                s32 kind = Player_80032610(v, side);
                bool archive, hat;
                if (kind < 0 || kind == Ft_Kind_Kirby) {
                    continue;
                }
                archive = ftKb_Init_803CA9D0[kind].filename == NULL || ((HSD_Archive**) &ft_80459B88)[kind] != NULL;
                hat = ftKb_Init_803CB3E8[kind] == NULL || ftKb_Init_803C9FC8[kind][color].joint != NULL;
                OSReport("squad-test: kirby P%d copy of P%d kind %d at frame %u: archive %s, hat %s\n", k + 1, v + 1,
                         (int) kind, gmSquad_Frame(), archive ? "ready" : "MISSING", hat ? "ready" : "MISSING");
            }
        }
    }
}

void gmSquad_OnVsFrame(void)
{
    s32 slot;

    if (!gmSquad_Live()) {
        return;
    }
    for (slot = 0; slot < GMSQUAD_PORTS; slot++) {
        GmSquadMember* m;
        if (!s_held[slot] || s_held_frame[slot] == gmSquad_Frame()) {
            continue;
        }
        s_held[slot] = false;
        m = due_member(slot);
        if (m != NULL) {
            swap_in(slot, m);
            s_current[slot] = (u8) (m - s_lineup[slot]);
            OSReport("squad: swap P%d -> %d:%d at frame %u\n", slot + 1,
                     m->ckind, m->color, gmSquad_Frame());
            test_kirby_copies();
        }
        s_respawning = true;
        fn_8016719C(slot, 0);
        s_respawning = false;
    }
}

/* The program checks this at load: a Squad Strike program with a library built without it (or the
 * reverse, or another squad version) is a mixed install and would desync, so it stops instead. */
MU_EXPORT int mu_squad_build(void) { return MU_SQUAD_VERSION; }

#endif
