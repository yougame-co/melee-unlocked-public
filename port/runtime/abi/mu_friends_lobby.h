/* The 4-player Friends lobby's build capability. NP_MSG_MU_BUILD's capability byte is full (0x01
 * early inputs, 0x02 pings, 0x04 Static, 0x08 rules digest, 0x10 Squad Strike, 0x20 pick previews,
 * 0x40 setup hash, 0x80 Squad Strike friend rooms), so this one rides a second byte that trails the
 * shield colour bytes. Builds before it stop reading at the shield bytes and send no second byte:
 * absent means 0.
 *
 * A lobby match plays a setup the room froze, with CPUs and rules every machine builds on its own,
 * then compares by setup hash before frame 1. A peer without this bit (or without the setup hash)
 * would build another match without knowing it: it is refused, never played. The room's
 * compatibility key (friends-lobby-1) keeps such a build out of the room in the first place; this is
 * the second lock, on the peer link itself.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef MU_FRIENDS_LOBBY_H
#define MU_FRIENDS_LOBBY_H

#define MU_BUILD_CAPS2_LOCAL_PARTNER 0x02 /* versioned machine pad packets and two owned human ports */
#define MU_BUILD_CAPS2_FRIENDS_LOBBY 0x01   /* plays the Friends lobby's frozen setup (yougame_lobby_setup.h) */
#define MU_BUILD_CAPS2_GONE_OWN_FRAMES 0x04 /* reads a player who left by their own frames (slippi_gone_port.h) */
#define MU_BUILD_CAPS2_ARENA 0x08          /* takes arena room messages (NP_MSG_MU_ARENA); upstream sends caps 0x10 */
#define MU_BUILD_CAPS2_AUTO_LCANCEL 0x10    /* presses for L-cancels this session; upstream sends caps 0x20 */
#define MU_BUILD_CAPS2_GECKO 0x20          /* takes Gecko code messages (NP_MSG_MU_GECKO); upstream sends caps 0x40 */
#define MU_FRIENDS_LOBBY_OLD_BUILD_TEXT "A player's game can't play the Friends lobby yet. Everyone needs the latest Melee Unlocked."

static inline int mu_local_partner_capable(unsigned caps2) { return (caps2 & MU_BUILD_CAPS2_LOCAL_PARTNER) != 0; }
static inline int mu_gone_own_frames_capable(unsigned caps2) { return (caps2 & MU_BUILD_CAPS2_GONE_OWN_FRAMES) != 0; }

/* One peer in a lobby match: 1 plays, 0 wait (its build message has not come), -1 refuse. After the
 * build wait a peer that never sent a build is refused too (Slippi Dolphin sends none). */
static inline int mu_friends_lobby_verdict(int received, int setup_hash, unsigned caps2, int waited)
{
    if (!received) return waited ? -1 : 0;
    return setup_hash && (caps2 & MU_BUILD_CAPS2_FRIENDS_LOBBY) ? 1 : -1;
}
#endif
