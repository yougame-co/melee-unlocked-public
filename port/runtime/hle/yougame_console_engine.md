# Console engine integration

`yougame::console::console_begin(link, config)` must run before `source_port::run`.
It accepts an already connected link and never calls `start_session`. It rejects late entry,
replay/online-test/P2P launches, invalid rosters/delays/ids, missing `YOUGAME_DATA`, and nonempty
or symlinked session card directories. The link interface `yougame_console.h` is unchanged.
The host/game ABI is now 26/9; ship both rebuilt binaries together.

## Console match rollback, Round C

Default behavior remains lockstep in every scene. `kConsoleRollbackDefault = false` in
`yougame_console_rollback_default.h` is the single build default; a rollback test branch changes
that one line to `true`. Exact `MELEE_CONSOLE_ROLLBACK=1` and `MELEE_CONSOLE_ROLLBACK=0` values
override either build default; unset/other values use the build default. The variable is read only
by `begin_console`; it has no effect on Slippi launches. Source `agent-test.json` accepts the strict
boolean `"consoleRollback": true` or `false`, which sets that environment value through the existing
startup defaults path (including non-hook builds). Static Recomp reports the key as inapplicable.
Use matching builds and the same switch on every console peer. Session start logs exactly one
`console: rollback on` or `console: rollback off (lockstep)` line. Only an ON frame-zero digest
mixes the fixed rollback marker; OFF digests retain their legacy bytes. A frame-zero mismatch logs
`console: rollback setting differs between seats` before the normal desync and session-end lines.
That diagnostic also appears for other frame-zero mismatches; the unchanged wire carries only a
combined digest, so it cannot distinguish a setting mismatch from other initial-state differences.
There is no new room or wire field.

`mu_console_match_setup` arms only the determinism subset before VS stage/fighter setup.
It preserves player rules, held pads and the session RNG, and leaves Slippi active/pending/mode,
B3 setup, General Codes, RNG processes, recording and reporting on their original paths.
The first actual GS_VS (or GS_SUDDEN_DEATH) body after on_enter starts a fresh host snapshot pool
and match epoch. Its input is confirmed; prediction starts with body 1. Snapshot and input keys
use the scene's total-body `unk_0`, which advances while the gameplay `unk_8` is paused.

Engine input ownership uses continuous session IDs. Local samples are immutable and retained;
input delay is applied only at issuance. A rewindable simulation cursor is separate from the
admitted frontier. Per-port contiguous confirmation advances only across exact received frames,
independently of ACKs or out-of-order future pads. The adapter orders remote ports ascending,
skipping local; absent ports supply confirmed disconnected pads. Histories retain 1024 past
frames plus issued future pads, within the link's 2048-frame retention.

The native predictor and its fourteen-entry prediction rings are shared with Slippi. Console
comparison includes every controller field, including analog buttons, presence and trigger
values below Slippi's deadzone. Admission permits seven unresolved bodies, then waits with the
existing responsive link deadline before a checkpoint could be evicted. Prediction comparison
uses confirmed inputs through the preceding body, fitting all seven history entries; current-body
input injection then refreshes the adapter. Earliest corrections across remotes select the load.

The earlier online body hook owns console match input admission and replacement. The later TE
console hook returns without touching that queue. B1/B2 retain their 32-byte payload and native
snapshot engine. Replay restores game memory, renews pads from retained history, rebuilds the
seven checkpoints and runs the existing camera/bone refresh between bodies. Queue tickets have
no rumble side effects during rollback. Historical injection writes the raw queue directly, then
captures the checkpoint, then interprets rumble exactly once per simulated body. SDK alarm renewal
skips rumble interpretation from console setup through terminal commitment, including body zero admission. Slippi/OFF keeps SDK renewal. It never advances
the session, samples hardware, sends input or waits for VI. Periodic pad alarms cannot inject
match samples. Missing required checkpoints log the epoch/body and end the console session
without a crash. Terminal commitment returns the engine to lockstep and releases its snapshot pool before scene teardown.

Match clocks have two domains. At the corrected pre-body boundary the host opens a simulation
clock context; game tick reads and explicit polls each add 2048 ticks on both first execution and
replay. Explicit in-body VI waits add a logical VI without waiting for presentation. Completing a
body adds one 60 Hz period and one logical retrace. Native checkpoint keys also retain the clock
at historical pad injection, including its logical VI deadline/count. Loads restore that context
and discard future candidates; replay rebuilds the clock checkpoints with the game checkpoints.
Between bodies the simulation clock stays fixed. Physical VI pacing uses the host's retrace count.

Hardware alarm service, deferred interrupt callbacks and audio reconciliation use a separate,
nonrewinding clock. SDK absolute/periodic registrations translate the caller's clock domain to that
hardware clock without consuming simulation ticks. The existing alarm hold/release contract stays
intact across loads. After commitment, menu/loading clock reads and poll-driven logical retraces
continue from the corrected simulation clock and its deadline, independent of hardware deadlines.
The switch-OFF clock path is unchanged. Rollback diagnostics use wall time or nonadvancing peeks.

The scene loop calls `mu_online_scene_terminal` after its terminal body and counter increments,
before honoring `unk_C`. It closes that body's audio/clock context, waits for authoritative inputs
through the body (without issuing another frame), and compares predictions. A correction re-enters
the retained scene loop at the earliest checkpoint. If it cancels the exit, normal execution resumes;
if replay ends earlier, that earlier body becomes the terminal boundary. Commitment finishes terminal
audio, retires voices from discarded future bodies, reconciles virtual-port rumble, clears host
resimulation, publishes the exit diagnostic and switches to lockstep before returning from the loop.
Missing histories/clocks/checkpoints or a terminal wait failure end the session explicitly. Already
issued future pads remain authoritative through results/loading and the next fresh match epoch.

Audio is initialized once at each console epoch. Console-only gates use `unk_0`, including paused
and terminal bodies; Slippi retains `unk_8` and its original online identity. Replay adopts sounds,
stops discarded voices, defers music and reconciles rumble. Console music inside a body is deferred
until presentation service can run outside the simulation clock. Camera/bone work still runs on
intermediate replay bodies and native loads still mark renderer discontinuities. Console load/replay/
commit counters are separate from Slippi rollback counters.

Digest candidates are keyed by continuous session boundary, overwritten on replay and published
once only after the predictor/runner reports an explicit validated boundary through S. That
frontier is bounded by the earliest unchecked prediction and the replay cursor; receive/confirmation
alone cannot advance it. Loads trim it and abandon future candidates; replay follows the same rule.
An input arriving between prediction checking and boundary receive cannot publish a stale candidate.
`console: hash F <hex>` is emitted only at that single final publication, including replay corrections. Each match has entry/exit candidates as well as the usual
60-body periodic boundaries; lockstep periodic checks continue. Exit digest comparison can complete
later through the existing channel; it is not an extra peer acknowledgement barrier. Console candidates additionally include explicit end method, pauser, timer seconds/subframes,
stage, teams, timer rules, item frequency and six-port stocks/damage. The inactive diagnostic keeps
the original 80-word hash byte-for-byte; the extension is marked valid only during console rollback.
Coincident exit/next-entry boundaries share one publication under the unchanged frame-key wire format.
This is diagnostic coverage, not a complete hash of item state, secondary entities or game memory.

Window-free tests execute the production predictor, console runner and RXB serializer against a
deterministic integer scene body and the real native snapshot engine. They force depths 1–7,
asymmetric delay/jitter, first-send loss/retry and reordering beyond input delay, compare corrected
states with a fully confirmed baseline, and exercise pause keys, sparse ports, repeated epochs,
horizon stalls, immutable future issuance and missing-checkpoint failure. Round B adds canceled
terminal predictions, late stock corrections, earlier/replay exits, timeout and paused quit exits,
clock equality through results/loading despite unequal service counts, finalized digest publication,
real audio-reconciler execution, and nonrewinding SDK alarm schedules. Round C forces the precise
boundary-60 check/receive race on forward execution and again during replay, verifies one corrected
hash line per peer, checks rumble wait expiry across rollback depths 1–7, and pins OFF Slippi audio,
alarms, POD prefix/digest/layout and entry wiring against pre-rollback `eb04f3c`. Rumble script
interpretation is a wait-state adapter in the harness; the SDK alarm guard is covered by wiring. These tests are not a native
Melee match, winner/results, stage-loading, device presentation or cross-platform gate.

The following sections describe the default switch-OFF lockstep clock/policy contract.

## Frame boundary and scheduler

The original frame-only clock deadlocked the common scene loop: `gm_801A4D34` waits for
`lb_80019894()` to return a nonempty raw-pad queue before entering any body
(`sourceport/patches/melee-native.patch:115174`). Outside Slippi,
`mu_online_engine_gate` preserved that wait. The periodic pad alarm needs ticks to advance,
but only the body's `Engine::advance` advanced the old ticks. Neither the body queue replacement
nor its eight-second link deadline could run after the initial samples drained.

The console-only branch in `mu_online_engine_gate` now delegates to `mu_console_engine_gate`
in `mu_pad.c`. The latter discards old raw samples and requests `VIWaitForRetrace` if no VI
has occurred since its previous admission. A VI already requested by rendering or loading is
reused, avoiding an extra presentation wait. The normal pad alarm supplies a queue ticket; if its
sampling period changed or it is canceled, the gate renews a ticket itself. It always returns
`*count = 1`, so the scene loop's local body-count loop cannot turn a loading backlog into a
batch of simulation bodies. Inactive sessions return before touching the queue or VI, and the
original Slippi/offline gate continues unchanged. No patch-file change is needed.

The patch calls `mu_te_frame_begin()` immediately before `lb_800198E0()` consumes the raw queue.
This hook is unconditional and precedes the pause condition, so menus, matches, and paused scenes
all enter it once per admitted body. Loading between bodies does not invent lockstep frames.
The shim's `mu_console_frame_begin` asks the host to snapshot the completed state, sample local
devices, and run `Engine::advance` for the next confirmed frame. It then replaces the ticket
with exactly those confirmed pads, before queue consumption. The first body is frame 0 and the
counter survives scene changes. PADRead only reads the cached composition; it never admits a body.

## Virtual game clock

Simulation frame IDs and VI counts are separate. Each native VI callback adds exactly 675000 ticks
(40.5 MHz / 60) and increments the session's VI count, before firing alarms and posting the game's
VI event. `h_vi_retrace_count` and `h_vi_next_field` expose that count, so boot/loading waits for VI
can complete without advancing a simulation body. `h_vi_wait_retrace` retains `host::retrace` and
its existing real-time sleep/vsync path for presentation.

Each game clock read (`h_ticks`, hence OSGetTime/OSGetTick) and each explicit `h_poll` adds exactly
2048 ticks. Thus a pure OS-time spin, including one inside a body, terminates after a fixed number
of reads. Explicit polls deliver alarms using a non-advancing clock peek; when the virtual deadline
is reached they request VI, without consulting the host timebase. A new VI schedules its next
poll-driven deadline one period after its resulting clock. A guard prevents poll-driven VI
recursion during a native retrace. Boot time remains the agreed epoch converted to console ticks.

The clock therefore follows the game's call sequence: VI periods plus fixed read/poll steps.
It is monotonic during a session and independent of wall time, network waiting, CPU speed, and
presentation lateness. Identical call sequences produce identical values; it is deliberately no
longer just `frame * 675000`. Wall time is still used only for presentation pacing and link
liveness/diagnostics. The link wait pumps only app/window messages, never game clocks or callbacks.

At frame F the sample goes to F + delay. Each owner sends a connected-neutral initial delay
window; unowned ports have err -1. Peers must supply every owned port before proceeding. The
console-only sampler initializes empty routes as disconnected and gives actual controllers priority
over the keyboard fallback (`port/runtime/host/window.cpp:1933`); the compositor picks its first
connected route. A controller on adapter port 3 therefore drives the agreed virtual port.

The wait polls the link and pumps only window/app messages (`port/app/source_host.cpp:3479`),
in slices no larger than 5 ms. It never calls host::retrace, poll, audio ticks, or game callbacks.
Monotonic wall time is used only for waiting/diagnostics, with an 8000 ms frame deadline.

## Inputs from the host

| Input | Console policy and source evidence |
| --- | --- |
| Pads and presence | Cached confirmed composition; first connected local controller, owned remote pads, all other ports disconnected. `source_host.cpp:124`, `:2204`; `yougame_console_engine.cpp:107`, `:154`. Local UI capture, scripts, auto L-cancel, offline delay, practice pad edits and user Gecko application are bypassed. |
| OS clock and calendar | `OSGetTime` adds boot time and ticks; `OSGetTick` uses ticks (`mu_os.c:125`). The virtual clock above advances by fixed steps per game call. Boot ticks convert epoch_ms from Unix to the console's 2000 epoch. No local timezone or wall date enters these values. |
| VI counters and alarms | Count/field use session VI count/parity, separately from body frame IDs. Alarm delivery peeks the same virtual clock in poll/native_retrace. The scene gate collapses backlog to one ticket; wall-time audio idle pacing is disabled. |
| RNG | Low 32 bits of config.seed seed the native 32-bit RNG before main and again at the first body, after the SDK's boot reseeding. `source_host.cpp:1083`; `mu_entry.c:316`; `mu_pad.c:45`. Ordinary match and diagnostic reseeding are suppressed during a session (`source_host.cpp:2920`, `mu_entry.c:343`). No per-match online reseeding/rollback code is changed. |
| Save data | Fresh `{YOUGAME_DATA}/console/<session_id>/card`; never delete old saves to make it fresh. `yougame_console_engine.cpp:29`; `source_host.cpp:3468`, `:2551`. Ordinary-card mod startup/protection, profile-card copying, backup and GCI import are bypassed (`:391`, `:655`, `:2557`, `:2600`). Card timestamps use session time (`:2645`); slot A presence and create/created confirmations are fixed (`:2682`, `:3289`). Isolation persists after a loss. |
| Gameplay settings | Canonical game option words 0, 0, LOCKED_CONTENT; mod flags/display/audio options 0; picture widening 1; no CLI match override or harness scene. `source_host.cpp:1162`, `:1208`, `:1223`, `:1227`, `:1237`, `:1245`, `:2047`, `:2891`, `:2912`. Local volume/window/presentation remain local. |
| Page/practice commands | Coordinator tick is inactive in a console session (`native_practice.cpp:652`); mutating practice bridge calls are refused (`source_host.cpp:3490`); lab saves/loads, host CPU/AI and live skin cycling commands are suppressed (`:1719`). |
| Diagnostic boot/audio flags | Session test_getenv returns null (`source_host.cpp:3306`); the lag-test boot and alternate music path are disabled/canonical (`mu_lagtest.c:35`, `mu_audio.c:161`). Lab/TE options are disabled by the option words. |
| Hardware constants | Existing fixed sound mode 1, progressive/reset code/reset switch 0, native MEM1 size, NTSC TV format/no component cable stay fixed. `source_host.cpp:2890`; `sourceport/game/shim/mu_vi.c:25`. |
| Disc/content | Existing synchronous native disc reads and queued completions remain. Content, executable/ISA and explicit listing layers must agree; this round does **not** negotiate or verify their identity or settings_digest. |

## Digest and teardown

Every 60 frames, including frame 0, the engine hashes canonical uint32 words from MuStatePod
using 64-bit FNV-1a. No padding or pointers enter the hash. The added words cover the public
main/rules menu kind, previous menu, hovered selection and entering state
(`sourceport/game/shim/mu_snapshot.c:151`). Existing RNG, major/minor scene, match frame and
primary fighter fields remain included. A late peer digest is checked against the retained digest
for its original frame; missing digests also have an 8 s liveness deadline.

Loss, pad timeout, digest timeout or mismatch logs one end line and shows the existing screen_label
notice. Mismatch additionally logs the requested exact desync line. Once ended, all remote ports
are disconnected and the local first controller continues on our virtual port; the session card
stays mounted privately. A new process is required for another session. Networking and request intake remain owned by the existing link/launcher; the engine owns session
inputs, finalized diagnostics and clock contexts, while the game shim owns prediction and replay.

## Validation limits

Window-free engine tests exercise 721 paired frames, plus 180 paired clock frames with asymmetric
link waiting, sparse ports/delays, four-player composition, reordered/delayed reports, pad/digest
timeout, immediate loss, late mismatch, repeated reads, card isolation and byte-identical inactive
pad forwarding. The PAD target compiles the actual `mu_pad.c`, `mu_os.c`, and `mu_vi.c` against SDK
type stubs. Its raw ring adapter uses real PADRead, with real OS periodic/one-shot alarm delivery
and VI callbacks. The simulated common scene loop drains its initial queue, runs 240 bodies across
four scene/pause scenarios, injects loading backlogs and canceled alarms, verifies retrace reuse,
and checks fixed-count OS-time waits before and inside bodies. A loading-only poll loop also
advances VI without a body. Python checks cover production host/gate/body/card/settings wiring.
These tests are not a native build and the ring adapter is not the full HSD controller library.

The native main-menu -> VS -> results/return gate, pause/loading/card boot, actual UI/device sampling,
paired real links, full supervising gates, and Windows/Linux builds remain for the supervising session.

- Native integration must validate all scene paths and audio/presentation pacing, including retraces
  already requested by rendering/loading, disabled interrupts, and session-loss clock handoff.
- The virtual clock requires identical game-side call sequences. Conditional instrumentation that
  reads OS time also consumes fixed steps; cross-build/instrumentation equivalence is not proven.
- The digest is diagnostic rather than complete: CSS/stage cursor internals, items, rules and
  secondary fighter state are not all represented. Public main/rules selection state is covered.
- Content/settings negotiation and cross-ISA floating-point determinism are still open.
- The no-Slippi build uses the game's AX music path by design; do not pair different build types.

## Boot readiness and loading service (fix round)

The production link owns a service worker from completed link setup through teardown. It polls
room/datagram traffic and keepalives every 5 ms through synchronous content/library boot and
loading without advancing game time or simulation. Engine clock/poll waits also poll links.
The first body sends its frame-0 digest as readiness evidence. Live links require every peer's
frame-0 digest before body 0 can proceed; neutral boot inputs are not readiness. The barrier
has its own 60 s monotonic deadline, ending with “Your friend's game didn't start.”
Only after all reports does the link arm its 5 s silence timer; engine pad/digest waits then
use their ordinary 8 s deadlines. Waiting never changes virtual ticks or simulation frames.
Rooms interruption reasons remain raw in logs and map to “Your friend left.” for
left/away/kicked and “The host ended the session.” for host_ended.
