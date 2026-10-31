/* Replace only the live Friends row's label texture. Shared disc descriptors
 * and animation image tables (including Update) always remain untouched. */
#include <mu_native.h>
#include <melee/mn/forward.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/sislib_font.h>
#include <sysdolphin/baselib/tobj.h>
#include "mu_menu_label.h"

void mu_slippi_friends_label(HSD_JObj* label, int menu_kind, int selection)
{
    static unsigned char pixels[MU_LABEL_BYTES] __attribute__((aligned(32)));
    static HSD_ImageDesc image;
    static int ready;
    HSD_DObj* dobj;
    if (!mu_slippi_menus_enabled() || !mu_slippi_friends_available() ||
        menu_kind != MENU_KIND_8 || selection != MU_SLP_OPT_FRIENDS || label == NULL) return;
    if (!ready) {
        if (!mu_friends_label_render(pixels, (const unsigned char*) HSD_SisLib_FontAtlas,
                                    (const unsigned char*) HSD_SisLib_8040C680,
                                    (const unsigned char*) HSD_SisLib_8040CB00)) return;
        DISC_SET(image.image_ptr, pixels);
        image.width = MU_LABEL_W;
        image.height = MU_LABEL_H;
        image.format = GX_TF_IA4;
        ready = 1;
    }
    for (dobj = label->u.dobj; dobj != NULL; dobj = dobj->next) {
        HSD_TObj* texture;
        if (dobj->mobj == NULL) continue;
        for (texture = dobj->mobj->tobj; texture != NULL; texture = texture->next) {
            HSD_ImageDesc* original = texture->imagedesc;
            if (original != NULL && original->width == MU_LABEL_W &&
                original->height == MU_LABEL_H && original->format == GX_TF_IA4)
                texture->imagedesc = &image;
        }
    }
}
