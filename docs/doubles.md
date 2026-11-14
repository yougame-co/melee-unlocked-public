# Console Doubles, first build

Opt in with `-DMELEE_DOUBLES=ON` for both the host project and the separate
`sourceport/game` project. Default OFF adds no compile definition, menu row,
ticket fields, state fields, or game shim. `tools/build_linux.py --doubles`
passes the option to both projects; `port/mac/build_full.py --doubles` enables
the host and requires an independently built matching game library.

This uses the four-port console mesh/rollback session. It does not enter
Slippi Teams, find_match mode 3, mut codes, or yg-teams.

## Entry, play, and return

Online > Doubles opens a home, with the existing Friends waiting display and
`mpFriend` invite picker. START searches, Z invites, B cancels search (every
member may cancel) or leaves an idle home. One computer is Solo; two computers
are Duo. Three/four people are told to play Friends. No local-add or couch seats.

Each computer uses its own generation-bound search ticket and search socket.
The home remains open and pinged during admission and play. A home claim can
arrive before matched; that clears queueing without withdrawing the search.
An actual roster/generation withdrawal cancels the search socket.

Admission reuses `yougame_console_link.cpp`, including its four-port mesh,
latency agreement, same-build checks, rollback and frame digests. Ready carries
the current revision and selection epoch and repeats on changes. Only the
room-designated match host sends begin. Console Start freezes the four ports,
seat IDs and teams. The client refuses changed settings, missing Team Battle,
missing teams, duplicate transports, and anything except slots 0/1 on team 0
and slots 2/3 on team 1.

The dedicated pre-main, pre-save checkpoint starts all four computers with an
empty disposable save, the same seed and the same console frame-zero barrier.
The C-only continuation reruns main after resetting game spans, initial heap
blocks, ARAM, GPU, audio and clocks; it retains the host-side home/socket.
Friends retains its existing same-card CSS reset guard and checkpoint.
After return, memory-card writes remain isolated to the disposable session card
until the process closes, matching the existing console-session behavior.
The game uses a VS table with four human CSS doors and then the match, skipping
stage selection. Frozen rules and port teams are reasserted before and after
CSS input. The original VS table is restored on leaving VS. Finish reports use
all four frozen seat IDs, with the winning team's two seats first. Draws and
voids retain all seats. Result is observed before dissolution; the main thread
retires the engine/links before closing the match room and sending party-back
on the still-open home. The retained home is foregrounded and reported to the page before
party-back; the game returns to Online without automatic requeue. A home
teams-dissolved ends only the match; kicked/ended/roomClosed close the home.
Busy members cancel through Ready false before requesting a search ticket;
the simulation checks the menu, console, Slippi and replay gates again before boot.
Engine-end notifications are match-ended commands, ignored outside admission/play.
Ending Doubles clears g_console_boot so ordinary Friends can reset/join again.

## Rules

`yougame_doubles.h::digest` is shared by begin and Start validation. Its default
canonical record is `1:1:4:8:0:1:31:32:28:8:3`; FNV-1a-32 produces
`mu:teams:v1:64657f94`. The hash is a settings agreement key, not authentication.

- Team Battle on; four stocks; eight minutes; items off; friendly fire on.
- Ordered legal stages: Battlefield (31), Final Destination (32), Dream Land
  (28), Yoshi's Story (8), Pokemon Stadium (3).
- Stage index: the full unsigned 64-bit console seed modulo five.
- Items off maps to Melee's frequency 255 and an empty item mask.

## Test hooks

Only builds with both `MELEE_DOUBLES` and `YOUGAME_AGENT_TEST_HOOK` accept
`agent-test.json` keys `doublesHost: true` (open a home and immediately search)
and `doublesJoin: "homecode"` (join a home). For a Duo, `doublesDuoHost: true`
opens the home and automatically searches after the second computer joins.
The ordinary row opens an idle home
for inviting a Duo partner before START. Four `doublesHost` copies can exercise
Solo pairing without clicks. Keys are ignored outside source-port hook builds.

## Desktop/page integration contract

Native sends `bridge({type:"mpTicket", ...fields}, timeout)`. The stdio adapter
copies the entire JSON body, adds its correlation `id`, and writes one `yg `
JSON line. The loopback adapter copies the same body to POST `/sdk`. Neither
adapter filters ticket fields. The reference Player.tsx already has the MU
Doubles ticket/room hook: it reads camelCase teamsLobby and partySearch,
then translates them to the HTTP route's teams_lobby and party_search.
The launcher HTTP adapter makes the same translation under MELEE_DOUBLES
and preserves the md capability in replies. These paths are tested offline;
the page/desktop path has not been exercised with real game copies.

The page must preserve these ticket fields, without injecting empty/default
fields into profiles where they are absent:

| Field | Home | Search | Match admission |
| --- | --- | --- | --- |
| `teamsLobby` | 1 | 1 | 1 |
| `mu_doubles` | 1 | 1 | 1 |
| `mode` | teams | teams | teams |
| `queue` | private | casual | private |
| `players` | 4 | 4 | 4 |
| `room` | own/generated or invited home | absent | matched room |
| `party_room` | absent | retained home code | absent |
| `partySearch` | null | lobby.party.queueing.id | null |
| `party` | absent | `{id: generation, n: 2}` for Duo; absent for Solo | absent |
| `local_players` | absent | absent in this build | absent |
| `lobby` | version 1, minPlayers 2, maxLocalPlayers 2, compatibility | version 1, minPlayers 4, maxLocalPlayers 1, compatibility | version 1, minPlayers 4, maxLocalPlayers 2, compatibility |

Compatibility is exactly the existing Friends/console gameplay fingerprint,
including `-console4`. The page must also attach its existing trusted `slug`,
`native_build` and `player_id` identity fields. Bridge `type` and `id` are routing
metadata, not ticket profile fields. If supporting later couch clients, forward
an explicitly supplied `local_players: 2` only on search and omit party; this
native build never sends it. Never translate this to legacy `doubles`,
`party_size`, `party_local`, `match_room`, or a Slippi queue.

Invites reuse `mpRoom` and `mpFriend` with the home code. The picker sends
`mode: teams`, `players: 4`, `mu_doubles: 1`, `teamsLobby: 1` and
`partySearch: null`; ordinary Friends keeps its singles/two-player request.
Room reports include `roomEpoch`, `mode: teams`, `queue: private`, `size: 4`
and `nativeTeams: {v: 1, kind: party|match, home, phase}`. Epochs increase on
foreground switches and across runs. Clears carry `code: null`, empty `players`
and a newer epoch with no nativeTeams. These match acceptNativeTeamsRoom,
acceptNativeTeamsClear and cleanNativeTeamsSummary. All page room reports are
best effort: a timeout/refusal logs and leaves the sockets running.

The page's join push is only `{type: "join", room}`. Native now uses the shared
Friends invite intake, then diverts to a retained Doubles home when the ticket
reply grants `md: 1`, or a welcome describes four seats, private, mode teams.
It does this before ordinary Friends readiness, save transfer or CSS joining;
it never depends on a mu_doubles join marker. Explicit native doublesJoin and
the Doubles row already request the granted home profile directly.

**Remaining page integration for invite-Duo:** Player.tsx's `handle`,
`case "mpTicket"`, must resolve an invited Melee Unlocked room's authoritative
profile before minting an ordinary console/Friends ticket. For a verified MU
Doubles home it must use the effective request `teamsLobby: 1`,
`partySearch: null`, `mu_doubles: 1`, `mode: "teams"`, `queue: "private"`,
`players: 4`, `room: <invited home>`, and lobby version 1, minPlayers 2,
maxLocalPlayers 2 with the native compatibility. It must retain that home in
nativeTeamsSession with md:1 and return the route's md:1 to native (the existing
reply projection already preserves md). Detection must be authoritative and
leave ordinary Friends/tournament requests unchanged, not trust a join marker.
If reusing `/api/multiplayer/teams` for discovery, its room-profile response must
distinguish MU Doubles and perform the matching muDoubles:true home check:
the current generic Teams invitation response does neither. The supplied page
and room references are read-only, so that page/server change is not included.
With today's page, ordinary invite-Duo still cannot reach a welcome: its
ordinary console ticket is rejected by Rooms' profile checks. Native's granted
home join, Duo generation tickets, inviteable solo-home reports, four-computer
match admission and ordered return are covered offline; end-to-end invite-Duo
remains blocked on the page promotion above.

## Verification limits

Offline tests execute the actual console link with injectable SessionIO and its
real broker Stream; the production retained-home coordinator runs against a
fake socket/broker. Separate executable fixtures run the actual menu gate,
fixed-rule game callbacks and CSS setters without disc assets. Existing tests
remain unmodified.

The root macOS CMake project builds the window-free tests, not the full playable
host/game. This worktree has no prepared decomp sources or full-host generated
headers. Therefore a complete game-library build, visual CSS verification,
four real copies, and the external desktop/page path remain unverified. No
network, package installation, fetch or push was used.

## Round 2 regression coverage

`yougame_doubles_test.cpp` verifies the exact native ticket JSON, authoritative
invite routing before ordinary Friends setup, and Teams foreground/clear epochs.
`yougame_doubles_runtime_test.cpp` exercises two matches and searches in one
home, terminal delivery to both sockets, stale commands/run queues, busy members,
busy admission, refused page reports, cancellation before/after the match socket
opens, renew-ticket fields, return ordering and the retained md:1 grant check.
`doubles_wiring_test.py` compiles the production boot and launcher ticket paths,
checks Player.tsx's actual field reads when the supplied reference is present,
and verifies busy-scene/replay/Ranked/Friends refusal without restoring memory,
failed Engine::begin without termination, disc gating and g_console_boot cleanup.
Both feature configurations keep the existing tests.
