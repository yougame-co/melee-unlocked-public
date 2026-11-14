/* ARAM the native hosts back (MuHostApi::aram_size). The game sizes its animation heap (AllA)
 * from it. Squad Strike builds give 40 MiB so AllA holds four fighters a side
 * (MU_SQUAD_ONLINE_ALLA in mu_squad_wire.h); AllA keeps its start and the ARAM heap above it stays
 * 1 MiB, so nothing else moves. Every other build keeps 32 MiB, byte-identical. On YouGame a squad
 * build never meets a plain one: its online keys carry "sq" (yougame_squad.h) and its Friends console
 * key ends in "-sq" (yougame_link.cpp console_io), so the two sides share this value. A Slippi peer
 * can have the other value; ARAM is outside the rollback snapshots.
 * Kept out of mu_host.h, which is the ABI table and carries no build switches.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MU_HOST_ARAM_H
#define MU_HOST_ARAM_H

#ifndef MU_HOST_ARAM_BYTES
#if defined(MU_SQUAD) && MU_SQUAD
#define MU_HOST_ARAM_BYTES 0x02800000u
#else
#define MU_HOST_ARAM_BYTES 0x02000000u
#endif
#endif

#endif
