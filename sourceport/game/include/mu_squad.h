/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026).
 * Squad Strike idea: Chris's friend Tyler (Squad Strike Gecko codes).
 */
#ifndef MU_SQUAD_H
#define MU_SQUAD_H

/* Optional native Squad Strike. All state is in the game image and rolls back.
 * A nonempty lineup opts into the mode at runtime, only in stock singles VS
 * (including Friends' shared, synced VS character select), or the explicit
 * YouGame Unranked Squad Strike row with exchanged, canonical lineups.
 * Offline: R clears the last extra; holding L clears the lineup and restores normal VS.
 * The online row picks four fighters with A; B removes the last pick, holding B clears them.
 */

#include <stdbool.h>
#include <dolphin/types.h>
#include <melee/ft/forward.h>
#include <melee/gm/forward.h>
#include <melee/lb/forward.h>
#include <melee/mn/forward.h>

#if MU_SQUAD

#include <mu_squad_wire.h>
struct GameCache;
/* Explicit Online row. Virtual picker door 0 maps to the local controller;
 * negotiated game ports are imported only from the host's exchanged bytes. */
bool gmSquad_OnlineChosen(void);
bool gmSquad_LobbyChosen(void);
void gmSquad_LobbyChoose(bool chosen);
bool gmSquad_LobbyAdmission(void);
bool gmSquad_OnlineReady(void);
void gmSquad_GetBudgets(u32* main_bytes, u32* animation_bytes);
void gmSquad_TrimOnline(StartMeleeData* start);
void gmSquad_ApplyLiveStart(StartMeleeData* start);
bool gmSquad_OnlineEditable(void);
void gmSquad_OnlineChoose(bool chosen);
/* Friend rooms (Direct): Squad Strike picked for a friend room, by "Play a friend" on the Squad
 * Strike row (picked) or because the friend picked it (gmSquad_OnlineFriendJoin). */
void gmSquad_OnlineChooseFriend(bool friends_pick);
bool gmSquad_OnlineFriend(void);
/* The squad rules apply to this online mode: Unranked's squad search, or a friend room in Direct. */
bool gmSquad_OnlineLive(int mode);
/* Direct friend room character select, every frame until squads are on: asks the host whether the
 * friend picked Squad Strike; on yes turns squads on and opens the squad CSS. 1 when it switched. */
bool gmSquad_OnlineFriendJoin(void);
/* Online CSS entry: drops a Squad Strike choice this screen's mode or friend room doesn't carry
 * (a room joined from the character select or results keeps no earlier choice). */
void gmSquad_OnlineRoom(int mode);
/* Direct lock-in: false holds it ("Waiting for your friend's game..." until the friend's game is
 * heard from; squads on and four fighters needed once the friend picked Squad Strike). */
bool gmSquad_OnlineFriendLockIn(void);
/// The friend-room lock-in is held for the friend's game (not refused): the search goes on.
bool gmSquad_OnlineFriendHeld(void);
void gmSquad_OnlineCss(void);
bool gmSquad_OnlineSelect(u8 coin_ck, u8 coin_color);
/* Agent-test hook hosts only: the squadLineup the host gave for this CSS visit, once. */
bool gmSquad_OnlineAgentLineup(MuSquadSide* out);
bool gmSquad_OnlineBudget(void);
/* Friends' synced select (a console session): want arms the host's costume rule (a skin over its
 * fighter's fixed size opens the disc's copy), sending the costume facts first when the host has
 * none; !want disarms it. The host's answer: 1 armed, 2 or 3 a standard copy is over its size (so
 * the fixed budget cannot hold), 0 off or no answer. */
u8 gmSquad_ConsoleCostumes(bool want);
bool gmSquad_OnlineSearchReady(void);
const char* gmSquad_OnlineError(void);
void gmSquad_OnlineNeedLineup(int count); /* "Pick 4 fighters first (n/4)." */
void gmSquad_OnlinePickCount(int count); /* clears that line once the count changes */
const char* gmSquad_OnlineTrimNotice(void); /* "" unless setup trimmed the squads */
u32 gmSquad_FileSize(const char* name, int* entry);
/* A fighter's worst vanilla costume (use GMSQUAD_FILE_COSTUME) or Kirby hat (GMSQUAD_FILE_HAT),
 * 0 when it has none. Constants, never the local disc: skins can never move them. */
u32 gmSquad_CostumeBytes(int kind, int use);
void gmSquad_OnlineSetup(StartMeleeData* start);
int gmSquad_OnlineTest(const int* picks, int n);
int mu_online_harness(void); /* shim/mu_online.c: the --online-test harness only */
int mu_online_launch_query(void); /* shim/mu_online.c: inside the host's launch answer */

#define GMSQUAD_SIZE MU_SQUAD_LINEUP /* fighters per squad, the coin pick included: exactly 4 */
#define GMSQUAD_EXTRA (GMSQUAD_SIZE - 1)
#define GMSQUAD_STOCKS_PER_FIGHTER 1
#define GMSQUAD_PORTS 4
#define GMSQUAD_NONE 0xFF

typedef struct GmSquadMember {
    u8 ckind; ///< ::CharacterKind, or GMSQUAD_NONE
    u8 color;
} GmSquadMember;

/// Squad members after the coin pick, per CSS port, packed from index 0.
extern GmSquadMember gmSquad_rows[GMSQUAD_PORTS][GMSQUAD_EXTRA];

/// Where the coin pick goes in each port's fighting order: how many of
/// gmSquad_rows were added before the coin was dropped (so a squad fights
/// in the order it was picked), or GMSQUAD_NONE while it has no coin pick.
/// Kept up by character select (gmSquad_TrackCoin).
extern u8 gmSquad_coin_pos[GMSQUAD_PORTS];

/// Character select, every frame per open door: has_coin_pick is whether
/// the door has a coin pick now.
void gmSquad_TrackCoin(int port, bool has_coin_pick);

/// The squad in fighting order: the coin pick (coin_ck, coin_color; left
/// out if GMSQUAD_NONE) placed among gmSquad_rows. Returns how many.
int gmSquad_Lineup(int port, u8 coin_ck, u8 coin_color,
                   GmSquadMember out[GMSQUAD_SIZE]);

/// Runtime mode gate: stock GM_VS (offline or Friends); callers also gate singles.
bool gmSquad_ModeAllowed(void);
int gmSquad_Count(int port);
bool gmSquad_Any(void);
/// Stocks a squad match gives everyone: the biggest squad's size.
int gmSquad_StocksFor(void);
void gmSquad_Clear(int port);
void gmSquad_ClearAll(void);
/// Changes whenever any squad changes; the preloader compares it.
u32 gmSquad_Key(void);
/// Scene entry drops match state; CSS rows persist across VS rematches.
void gmSquad_Disable(void);
bool gmSquad_Live(void);
u32 gmSquad_Frame(void);
u32 gmSquad_Random(int port, int salt);
/// Private player mapping exposed through a tiny guarded decomp hook.
s32 mu_squad_fighter_kinds(CharacterKind kind, s32* extra);
/// Heap sizing at boot, exclusively in squad-enabled builds.
u32 mu_squad_main_heap(u32 original, uintptr_t lo, uintptr_t hi);
u32 mu_squad_animation_heap(u32 original, uintptr_t lo, uintptr_t hi);
u32 mu_squad_sound_heap(u32 original, const u32 banks[8]);
int gmSquad_StockRule(bool active, int current, int need, int* saved, int* applied);
/* Enumerate exactly the archives requested by the fighter/copy preload paths. `use` says what
 * the file is: costumes and Kirby's colour hats are cosmetics a skin may replace. */
#define GMSQUAD_FILE_MAIN 0
#define GMSQUAD_FILE_ANIMATION 1
#define GMSQUAD_FILE_COSTUME 2
#define GMSQUAD_FILE_HAT 3
typedef void (*GmSquadFileVisitor)(void*, const char*, int use);
void mu_squad_fighter_files(int kind, u8 color, void*, GmSquadFileVisitor);
void mu_squad_copy_files(int kind, u8 color, void*, GmSquadFileVisitor);
void mu_squad_effect_file(int index, void*, GmSquadFileVisitor);
bool gmSquad_Budget(const GmSquadMember* members, int count, int stage);
/// Sound banks of every squad member (lbAudioAx_80026E84 masks).
u64 gmSquad_SoundMask(void);

/// lbDvd_80017960: request squad members' files (and Kirby's copies).
void gmSquad_AddPreloads(struct GameCache* cache);
/// gmVsMelee_EnterVs: decide whether this match is a squad match and give
/// everyone gmSquad_StocksFor() stocks if it is.
void gmSquad_OnEnterVs(StartMeleeData* start);
/// fn_8016E124 (end of VS setup): initialise every
/// squad member's preloaded files while the match is still in its lockstep
/// setup, so no archive is first parsed mid-match.
void gmSquad_WarmFiles(void);
/// fn_8016719C: true to hold this respawn because a squad member is due.
bool gmSquad_HoldRespawn(s32 slot, s32 subchar);
/// fn_8016A4C8 (every VS frame): swap held players and respawn them.
void gmSquad_OnVsFrame(void);
/// The HUD's stock icon i (0 = leftmost) for this player: the squad member
/// who will be fighting by then, in their colour. False for the game's own
/// icon (icon 0, or no squad).
bool gmSquad_StockIconFrame(int slot, int i, f32* frame);

#endif

#endif
