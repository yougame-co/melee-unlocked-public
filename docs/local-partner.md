# Doubles local partner

The Doubles host's second physical controller claims open seat 1 with START. Its own cursor sends
hover, pick and unpick for that seat; a fresh B press without a pick releases it. The authenticated
view gates both routing and UI. Friends, guests, searching and started setups never accept this
controller's lobby actions. CPU and invite controls exclude a local seat.

Frozen human seats carry connectionId and localIndex. A local partner has index 1, the adjacent
human port after its owner, the same team and copied nonempty digests. The setup parser and plan
validate these fields; participants and transport rosters remain one row per connection.
Results rank the partner's exact frozen id, local:<connectionId>:1.

Every Doubles queue search advertises local_partner using kLocalPartnerCapability, including solos
and online friend pairs. A local pair uses the solo ticket shape (maxLocalPlayers 1, no party or
local_players); the room knows the pair from seat 1. The match-admission request also advertises it.

## Count audit

| Path / loop | Unit | Treatment |
| --- | --- | --- |
| Plan.humans / ports / local_ports | Ports | Both humans count; CPUs follow humans. |
| Plan.machines / match_roster / connection_seat | Machines | One row per connection, owner's port as slot. |
| bounded_remote_machine_count / RemoteMachineCount | Machines | Real connections only. |
| bounded_remote_port_count / RemotePortCount | Ports | Every human except this machine's main port. |
| should_skip_online_frame / stall counters | Ports | Local virtual partner is excluded from stalls. |
| prepare_opponent_inputs / history / despawn | Ports | Both ports read confirmed queues; a dead owner drops both. |
| Checksum comparisons | Machines | Checksum travels with the machine packet; port replies share it; host compares only the primary. |
| CalcTimeOffsetUs / frame_offset_data / ping | Machines | One clock and ping per connection. |
| SendSlippiPad ACK minimum / ACK timers / resend | Machines | A shared ACK covers both input streams atomically. |
| PlayerIdxFromPort | Machines | Looks up owner's connection index; never arithmetic across virtual ports. |
| GetSlippiRemotePad / PortInputs::port | Ports | Virtual indices skip the main local port. |
| player_active / ForceDisconnectPlayer | Machines | Port activity resolves through its owner. |
| GetActivePlayerIndices / pre-match activity check | Ports | Includes local partner and remote partners. |
| Selection readiness / build verdict | Machines | Lock-in once per real connection; frozen picks populate all human ports. |
| setup_hash_gate / peer hashes | Machines | One setup hash per real connection. |
| setup_block_hash / frozen setup digest | Ports | All human and CPU records plus ownership metadata are covered. |
| ENet connections / candidates / links / failures | Machines | No virtual transport entry. |
| Names / identity reply / result placement | Ports | Look up by actual setup port; peer lists stay per machine. |
| Early input / ring | Machines, main port only | No partner early stream; virtual ports use confirmed input. |

The machine pad message is 0xE5, magic MUL, version 1. A fixed header identifies the main and optional
partner port, common frame, checksum and history length; newest-first pad bytes are interleaved.
In a match containing a pair, every machine uses this format, even a solo. Receiving binds the
sender to the real connection, validates ownership, exact size, version and both histories before
updating either queue, then ACKs once. A build without the second-byte local-partner capability is
refused before play; old 0x80 pad packets are rejected in such matches.

The owner samples both physical pads on new frames only. Both target the same delayed frame.
The partner enters a confirmed virtual queue before the game reads the reply, and the existing
remote-input history supplies it during rollback. Its resend history is separate from the rollback
frontier: a slow connection's ACK cannot make the owner read stale partner pads or predict them.
The local read walks back from the requested frame, excluding future delayed pads, even at the
full rollback window.
Early-input/ring still samples only the main pad. The native predictor regression verifies that the
partner stays confirmed without any early samples while other ports predict, and replays its bytes.

## Offline verification and limits

The local-partner simulation uses the production packet/queue helpers for three machines and four
ports, two local pairs, and a pair versus solo plus CPU. It covers dropped and reordered packets,
shared ownership/activity, malformed and old packets, and independent ACK/rollback retention.
The controller test compiles the actual pad router with a fake view and pads. The setup tests cover
ownership, team and digest refusals, identities, machine counts and result ranking.

This does not run the game or live ENet peers. No rendered CSS, physical controllers, two-machine
rollback/desync behavior, disconnect timing or live Rooms interoperability is proven here. Those
need a real two-machine run after the server half is available. Static recomp's early input and
single-pad EXI path remain unchanged; this feature's physical second-pad path is native game only.
