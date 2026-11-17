/* The stage's own map objects, tagged for the renderer's looks (gx_core.h lightweight_background):
 * which map gobj of the loaded stage a draw comes from, and whether that gobj is scenery (trees, the
 * backdrop, props off the play plane) rather than a floor, wall or platform. The renderer cannot tell
 * them apart by depth on stages whose scenery stands on the play plane (Dream Land's Whispy, Fountain
 * of Dreams' trees, Yoshi's Story's clouds and light shafts); the game knows which object is which.
 * Display only: the tag rides in the GX stream behind the owner tracking switch, nothing in the game
 * state changes. */
#include <melee/gr/types.h>
#include <melee/gr/ground.h>
#include <sysdolphin/baselib/gobj.h>
#include "../../../port/runtime/abi/mu_stage_scenery.h"


/* The byte after token 0xF7: the map gobj index (0..0x3F), 0x80 set for scenery, 0x40 for a map whose
 * props stand close behind the play plane. */
int mu_stage_map_tag(void* ground)
{
    const Ground* gp = ground;
    const int map_id = (int) gp->map_id;
    if (map_id < 0 || map_id > 0x3F) {
        return 0x3F;
    }
    return (mu_stage_map_is_scenery((int) stage_info.grkind, map_id) ? 0x80 : 0) |
           (mu_stage_map_is_tight((int) stage_info.grkind, map_id) ? 0x40 : 0) | map_id;
}
