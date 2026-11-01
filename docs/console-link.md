# Console link contract (integrated game side, 2026-10-31)

Console rollback Round C keeps this entire link/wire contract unchanged. Use
`MELEE_CONSOLE_ROLLBACK=1` or Source agent-test.json `"consoleRollback": true` on every peer
before launch to enable native VS prediction/replay. `MELEE_CONSOLE_ROLLBACK=0` (or the JSON
boolean `false`) forces lockstep even in a test build whose single `kConsoleRollbackDefault` line
is true. The shipped default is false; menus/loading/results always use lockstep.
Host/game ABI 26/9 must be rebuilt together. Frame zero tags ON digests to reject mixed seat
settings before gameplay; OFF digests keep their legacy bytes. Start logs show the setting, and
final `console: hash F <hex>` lines can be compared across seats. Round C fixes correction-frontier
publication and rumble checkpoint ordering; actual native match/device validation is still pending.
See `port/runtime/hle/yougame_console_engine.md`.

The Source host consumes `take_request()` after agent defaults, bridge hello and pending-file
intake, then calls `start_session()` and `console_begin()` before `source_port::run()` and frame 0.
Setup runs on a worker; the host waits in 5 ms slices and pumps only `host::window_pump()`.
Caps are emitted at bridge/window creation; queued agent/app launches and hello launch intake
explicitly emit the existing `yg {"ready":true}` once before their blocking setup wait. Ordinary launches still send ready on first
visible present. The screen-label notice draws only during frame rendering, so this path cannot
draw “Connecting to your friend…” before frame 0 without new UI. Startup failure logs a reason,
queues the existing screen-label notice and continues local play.

## Intake and human entry

Hook-only Source agent requests remain:

```json
{"console":{"room":"new","players":4}}
{"console":{"room":"ABCDEF","players":4}}
```

They cannot be combined with agent `online` or `match`. App hello.launch and consoleSession pushes
accept the full relaunch request below. A sessionId requires all frozen launch fields; partial
session launches are refused. Rooms is authoritative: every supplied field must match the frozen
object. Seed is normalized losslessly to uint64 inside C++, and emitted as hex to the app.
Requests accept players 2..4, ports 1..players, inputDelay 1..8, nonnegative int64 epochMs,
1..64 ASCII alphanumeric/underscore/hyphen session IDs, and `new` or 4..16 alphanumeric room codes.
Only a fresh request may use `new`. Intake gives app pushes precedence over hello and pending files.

On this native Source branch, Online > Friends and YouGame room invites reuse the existing
picker (`mpFriend`, mode singles, players 2), room code and join intake. The accepted room enters
console waiting instead of Slippi CSS. The existing `##yougame_friend_waiting` panel shows
`Waiting for your friend...`, then `Press START to play together` for a full host room or
`Waiting for the host` for a guest. A fresh START after release is required; the invite press cannot
start the session. Z cancels. The panel owns game-pad input while waiting. Typed Direct codes,
Static Recomp Friends, Unranked, Ranked and Slippi transports retain their existing behavior.
Human entry is two-player in this first milestone; agent/launch sessions support 2..4.

When Rooms freezes the session, a game whose app advertises `relaunch` in its caps array (either
a `yg {"caps":[...]}` line or hello caps) emits exactly:

```json
{"type":"relaunch","request":{"type":"consoleSession","room":"ABCDEF","sessionId":"session-fixed","port":2,"players":4,"seed":"fedcba9876543210","epochMs":1793443200123,"inputDelay":3}}
```

The wire prefix is `yg `. The app relaunches with `hello.launch` or a consoleSession push containing
the inner request. Without that capability, the game writes `{YOUGAME_DATA}/console/pending.json`:
the same inner request plus numeric `createdAt` (local Unix ms) and `build` (FNV-1a-64 fingerprint
of the actual host executable, hex). It shows:
`Ready! Close the game and press Play again to start together.`
Next boot accepts only age 0..89999 ms and the same executable fingerprint. Expired, future-dated,
malformed, symlinked and other-build records are ignored. A valid pending file is removed under
the launch-queue lock before publishing its request. A failed delete never publishes it. An app
request already queued wins and leaves the pending file untouched. No ordinary save is deleted.
The original waiting process keeps its room open until close/cancel or session end.

## Rooms messages (authoritative shape)

All traffic uses the existing app broker and helper; there is no native socket fallback.
The ticket shape remains:

```json
{"type":"mpTicket","room":"new","players":4,"mode":"console","ranked":false,"lobby":{"version":1,"minPlayers":4,"maxLocalPlayers":1,"compatibility":"<Direct compatibility>-console1"}}
```

Joining uses the room code. The answer supplies ticket and room. Console uses the canonical native
Direct content key in both processes, before ordinary mod/card discovery, with no ranked rules.
Content/settings identity beyond that key remains assumed, not negotiated.

Broker envelopes are `{type:"netOpen",ticket,room}` -> `{conn}`, net pushes with
`event:"open"|"message"|"close"`, and `{type:"netSend",conn,data:JSON.stringify(message)}`.
Welcome supplies `me`, `host`, `revision` and participants with unique id, connectionId and slot,
with localIndex 0. Before start, slots must fill 0..players-1 before the host can begin.
Only the host sends today's Friends begin extended as follows, retrying idempotently every 2 s:

```json
{"t":"begin","revision":7,"console":true,"inputDelay":3,"settingsDigest":""}
```

The client sends neither sessionId nor seed. Rooms makes both and freezes:

```json
{"t":"start","console":{"sessionId":"session-fixed","seed":"fedcba9876543210","startedAt":1793443200123,"inputDelay":3,"settingsDigest":"","participants":[{"slot":0,"port":1,"id":"p0","connectionId":"c0","user":"user0","name":"Player 1"},{"slot":1,"port":2,"id":"p1","connectionId":"c1","user":"user1","name":"Player 2"}]}}
```

Seed must be exactly 16 lowercase hex digits; startedAt is server Unix ms and becomes epoch_ms.
Delay must be 1..8. The game sends an empty settingsDigest; received digests follow Rooms'
128-character ASCII letters/numbers/colon/underscore/hyphen contract. Ports are read from
console.participants[].port and must equal slot+1. The console roster is authoritative; an optional
legacy top-level participants array is only a cross-check of slot/id/connectionId/localIndex.
No `version`, client seed/sessionId, or `epochMs` is required in start.console.

After start, a seat may drop its socket for ~60 s during relaunch; Rooms retains the session 90 s.
The second boot requests the same ticket/room and opens with the optional session hint:
`{type:"netOpen",ticket,room,session:"session-fixed"}`. The app must put this hint on Rooms' join
as the WebSocket query `session=<sessionId>`, using a normal authenticated ticket with the
**same participant id (`p`)** as the first boot. This is not a ticket field or a subsequent hello.
Authentication and stable identity remain app-owned; the game does not generate a replacement.
Console teardown sends a clean close (1000), never an explicit `{"t":"leave"}`. A leave
would interrupt the session immediately, including during relaunch grace.
Rooms resends the frozen console object (on start or welcome) and relays candidates again.
The client checks sessionId, seed, startedAt/epochMs, delay and its assigned port against launch.
It never sends begin again. A 65 s candidate rendezvous accommodates the relaunch gap before
starting the usual shared 20 s link budget. Transient leave/away observations during that
rendezvous are ignored; sessionEnded remains definitive.

After setup, a dedicated service worker pumps room events, datagrams, ACKs and keepalives
every 5 ms through content initialization, library loading, boot and later loading waits.
It never advances simulation or game time. Engine OS-time/poll waits also poll the link.
Frame-0 state digests report that each peer reached its first body. No silence timer is
armed until our own report and every peer report have arrived. The frame-0 barrier has
a separate 60 s deadline and fails with “Your friend's game didn't start.” After it passes,
normal 5 s silence and 8 s pad/digest deadlines apply. Shutdown joins the worker before
tearing down the host/bridge.
A `command-error` for begin with `code: "console_mixed_builds"` logs that code and shows
“Your friend has a different version of this game.”
Fresh sessions wait up to 5 minutes for every seat and the start (a friend has to accept, launch and boot); ticket and room open each have a 10 s bound.

Each frozen connection receives the existing Friends compatibility candidate every second:

```json
{"t":"msg","to":"c1","d":{"kind":"mu-cand","v":2,"compat":"<compatibility>","code":"","match":"session-fixed"}}
```

The forwarded envelope supplies from. Every candidate must be from a frozen sender, name the same
session, and agree on compatibility. The helper owns ICE/relay addresses and credentials.
Attachment, directOpen and readiness share 20 s; individual attach/open calls have 2 s/1 s bounds.
Established links are serviced while the remaining mesh opens.

Rooms ends a session for all seats with:

```json
{"t":"sessionEnded","sessionId":"session-fixed","reason":"host_ended","port":1}
```

Reasons are `left`, `kicked`, `away`, and `host_ended`. The first three show
“Your friend left.”; `host_ended` shows “The host ended the session.” The raw reason is logged
and propagated via the existing `mu-console-end` message:

```json
{"t":"msg","to":"c1","d":{"kind":"mu-console-end","v":1,"sessionId":"session-fixed","reason":"host_ended"}}
```

Only a frozen sender and matching session can supply that second signal. Other link closures,
changed frozen membership or 5 s silence use `your friend disconnected`. No ranked results are sent.
Cleanup closes each helper link and the broker room with netClose code 1000.

## App link metadata and measured delay

The app's up push is:

```json
{"push":{"type":"direct","link":2,"event":"up","path":"lan","rttMs":12}}
```

Read path, never via. lan and direct are direct routes; relay is relay; missing/unrecognized path is
unknown and never prevents an otherwise up link from starting. Log the actual path, including lan:
`console: link to port 2 up via lan in 47 ms`.
A later `{type:"direct",link:2,event:"path",path:"direct",rttMs:8}` preserves link liveness, updates
RTT and logs once per change: `console: link to port 2 now direct`.
Each valid rttMs is one RTT sample, not the final agreed delay by itself.

Once every link is up, every machine pings every peer for about 500 ms. It reports its worst sample
to the frozen host over the links. The host takes the maximum across all reports, including its own,
and chooses `max(start.inputDelay, clamp(ceil((maxRtt / 2) / 16.67) + 1, 2, 8))`.
The host repeats its offer until every peer ACKs it, then repeats the matching commit for 3 s while
links are pumped. Peers ACK repeated offers and enter only on the matching commit. Reports/offers/
ACKs/commits are session-tagged and sender-checked. All machines update SessionConfig.input_delay
before console_begin and log once: `console: input delay 6 (worst rtt 144 ms)`.
The agreement deadline is 3 s after the measurement window. Missing samples, reports, host value
or commit fail startup with `couldn't agree on a delay`; there is no local delay fallback.

Startup control datagrams are 20 bytes: marker 0xc1, sender port u8, kind u8, delay u8,
session FNV-1a-64 u64, and value u64 (big endian). Kinds 1/2 ping/pong carry a local timestamp,
3 carries a worst RTT report, 4/5/6 offer/ACK/commit carry worst RTT plus delay. A pong must echo an
outstanding timestamp. These controls do not consume simulation frames or pad/digest serials.

## Datagram version 1

All multibyte integers are unsigned, big endian. No C struct padding is transmitted.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 1 | Version 1 |
| 1 | 1 | Sender port, 1..4, checked against the helper link |
| 2 | 1 | Pad count 0..16 |
| 3 | 1 | Digest count 0..48 |
| 4 | 8 | FNV-1a-64 of session id (stale-packet filter, not authentication) |
| 12 | 4 | Highest contiguous received pad frame, `ffffffff` means none |
| 16 | 4 | Highest contiguous received digest serial, `ffffffff` means none |
| 20 | 15 each | Pad: frame u32, buttons u16, stick X/Y and sub X/Y i8, triggers L/R u8, analog A/B u8, error i8 |
| after pads | 16 each | Digest: serial u32, frame u32, digest u64 |

Maximum emitted packet: 1028 bytes, below the helper's 1200-byte limit. Frame `ffffffff` is reserved.
Pads are monotonic from frame zero. Sampling sends a new pad directly to the helper ring; no later
pump is needed to flush it. Each data packet carries the newest eligible pad plus up to 15 oldest
eligible unacknowledged frames. A pad becomes eligible for retry 50 ms after its own last send.
Retry batches are also spaced by 50 ms, independently of new samples, digests and ACK traffic;
this coalesces staggered holes during loss recovery. Prioritizing old
holes prevents a burst loss longer than 16 frames from permanently stopping the contiguous ACK.
Receiving pad/digest data immediately sends a header-only ACK, including for duplicate data after
a lost ACK. It never replays our pending payload. ACK-only traffic does not create an ACK loop.
Idle keepalives remain at most 100 ms apart when pumped.
Incoming pads use a tagged 2048-frame ring; per-peer pending pads and digest queues are bounded
to 2048. Pending overflow ends the session. Digests have their own monotonically assigned serials
because digest frame numbers can be sparse. Each peer's ACK stops its resends; a digest is retired
after all peers ACK it. Reads are idempotent within retained history; duplicate/reordered packets
do not replace an already stored pad. Another version is dropped with one log per peer.

`pump` polls bounded batches and waits on the helper doorbell for at most min(wait_ms,10 ms),
allowing prompt room event checks. It returns without sleeping if the initial batch received a
datagram, and releases the link mutex across a doorbell wait so sampled sends and the background
worker can proceed. The background worker itself calls pump(0) and sleeps outside the mutex.
A room worker owns blocking bridge sends. Startup also services
datagrams so a peer still finishing its other mesh links can ACK and keep established links alive.

Every 600 sent pad frames logs each peer's last ACK latency estimate, retransmitted pad records,
and newly received out-of-order records. RTT includes retransmission/ACK scheduling delay; it is
not an ICE path ping. Counters reset each reporting window.

Offline coverage includes real broker Stream messages, all startup shapes, a simulated 60 s
relaunch gap without a second begin, missing path/via, route promotion, a four-instance RTT mesh
with its slowest pair away from the host and dropped agreement messages, boot fakes, launch caps,
pending expiry/build/consumption, and existing pad/digest reliability tests. Native builds, real
app/Rooms/helper sessions, actual relaunch, menu/VS/loading flows, cross-platform determinism and
full gates remain supervising-session validation. No server or network was contacted here.

The offline latency regression runs two real engines for 1200 bodies each at paced 60 Hz, with
the production service workers, delay 4, and a simulated 15 ms transit in each direction. It checks
every composed pad and reports each 600-body window's retransmitted records and first-send-to-ACK
RTT. Whole advance-call time provides a conservative upper bound on pad wait (it also includes
local processing and the initial readiness barrier). Gates require less than 500 ms per window,
RTT samples at most 60 ms, and fewer than 5 retransmitted records per body. It does not model the
real app/helper ring, TURN queues, rendering/capture contention, or cross-platform scheduling.
