/* Replace only custom Online label textures, preserving native row slots and highlight. Shared disc descriptors
 * and animation image tables (including Update) always remain untouched. */
#include <mu_native.h>
#include <melee/mn/forward.h>
#include <melee/mn/mnmain.h>
#include <melee/mn/types.h>
#include <melee/sc/types.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/sislib.h>
#include <string.h>
#include <dolphin/os.h>
#include <melee/lb/lbspdisplay.h>
#include <sysdolphin/baselib/gobjuserdata.h>
#include <sysdolphin/baselib/memory.h>
#include <sysdolphin/baselib/pobj.h>
#include <sysdolphin/baselib/tobj.h>
#include "mu_menu_label.h"

void mu_slippi_friends_label(HSD_JObj* label, int menu_kind, int selection, int capture)
{
    static HSD_ImageDesc* native[8];
    static HSD_ImageDesc image[3];
    int kind = mu_slippi_menu_label_kind(selection), slot;
    HSD_DObj* dobj;
    if (!mu_slippi_menus_enabled() || !mu_slippi_yougame_menu() ||
        menu_kind != MENU_KIND_8 || selection < 0 || selection >= 8) return;
    if (capture) {
        if (selection == 0) for (slot = 0; slot < 8; ++slot) native[slot] = NULL;
        native[selection] = NULL;
    }
    if (label == NULL) return;
    for (dobj = label->u.dobj; dobj != NULL; dobj = dobj->next) {
        HSD_TObj* texture;
        if (dobj->mobj == NULL) continue;
        for (texture = dobj->mobj->tobj; texture != NULL; texture = texture->next) {
            HSD_ImageDesc* original = texture->imagedesc;
            if (original != NULL && original->width == MU_LABEL_W &&
                original->height == MU_LABEL_H && original->format == GX_TF_IA4) {
                HSD_ImageDesc* replacement = NULL;
                /* All eight ORIGINAL descriptors are captured before any visible
                 * row is swapped, including Party (slot 4), which is hidden at
                 * the top level. Descriptors and image tables stay immutable. */
                if (capture) { native[selection] = original; continue; }
                if (kind == 1 || kind == 2) {
                    image[kind - 1] = *original;
                    DISC_SET(image[kind - 1].image_ptr,
                             kind == 1 ? mu_baked_friends : mu_baked_slippi);
                    replacement = &image[kind - 1];
                } else if (kind == 8) {
                    image[2] = *original;
                    DISC_SET(image[2].image_ptr, mu_baked_tournament);
                    replacement = &image[2];
                } else if (kind >= 3 && kind <= 5) {
                    replacement = native[2 + selection];
                } else if ((kind == 6 || kind == 7) &&
                           original == native[2 + selection]) {
                    /* Restore if the panel reuses a submenu row on Back.
                     * A newly created Ranked/Unranked row is never swapped. */
                    replacement = native[selection];
                }
                /* Ranked/Unranked (kinds 6/7) keep their native art, NO swap. */
                if (replacement && replacement->width == original->width &&
                    replacement->height == original->height && replacement->format == original->format)
                    texture->imagedesc = replacement;
            }
        }
    }
}

/* Use exactly the native cursor creation/animation path to capture all eight
 * Online slots, without touching texture animation internals. The image
 * descriptors belong to the loaded menu archive, not these temporary JObjs.
 * Run at menu creation, before the first custom row swap; recapture on return
 * so a reloaded archive never leaves stale pointers. */
void mu_slippi_menu_labels_init(StaticModelSyms* model, int menu_kind)
{
    int slot, track;
    if (!mu_slippi_menus_enabled() || !mu_slippi_yougame_menu() || menu_kind != MENU_KIND_8 || !model) return;
    mu_slippi_friends_label(NULL, menu_kind, 0, 1); /* clear an earlier archive, even if allocation fails */
    for (slot = 0; slot < 8; ++slot) {
        HSD_JObj* parts[7];
        HSD_JObj* cursor = HSD_JObjLoadJoint(model->joint);
        if (!cursor) return;
        HSD_JObjAddAnimAll(cursor, model->animjoint, model->matanim_joint, model->shapeanim_joint);
        /* These are the same cursor-parts indices used by mn_8022B3A0. */
        lb_80011E24(cursor, &parts[1], 3, -1);
        HSD_JObjReqAnimAll(parts[1], 0);
        HSD_JObjAnimAll(parts[1]);
        HSD_JObjReqAnim(parts[1], 140 + 2 * slot);
        for (track = 0xC; track <= 0x13; ++track) mn_8022F3D8(parts[1], track, TOBJ_MASK);
        HSD_JObjAnim(parts[1]);
        mu_slippi_friends_label(parts[1], menu_kind, slot, 1);
        HSD_JObjRemoveAll(cursor);
    }
}

/* Keep the game's text object, camera, position, box and font parameters.
 * Replace only its words, after copying the actual catalog stream's prefix.
 * The normal HSD_Text destructor owns both allocations, including notices
 * repeated while the user remains on the same row. */
void mu_slippi_menu_description(HSD_Text* text, const char* message)
{
    unsigned char encoded[1024], stream[128 + sizeof encoded];
    SisBuffer* alloc;
    int prefix, n, i, length;
    if (!text || !message || strlen(message) > 100) return;
    prefix = mu_menu_description_prefix(text->sis_buffer);
    if (!prefix) return;
    memcpy(stream, text->sis_buffer, prefix);
    length = HSD_SisLib_803A67EC(encoded, (u8*) message);
    n = prefix;
    /* The editable ASCII encoder inserts its own 0x0a/0x0b spacing overrides.
     * Keep glyphs/spaces only: the row's SIS prefix supplies the kerning. */
    for (i = 0; i < length;) {
        unsigned op = encoded[i];
        int size = op >= 0x20 ? 2 : mu_menu_sis_command_size(op);
        if (op >= 0x20 || op == 0x1a) {
            memcpy(stream + n, encoded + i, size);
            n += size;
        }
        i += size;
    }
    stream[n] = 0;
    alloc = (SisBuffer*) HSD_SisLib_Alloc(sizeof *alloc);
    if (!alloc) return;
    alloc->data = (u8*) HSD_SisLib_Alloc(n + 1);
    if (!alloc->data) { HSD_SisLib_Free(alloc); return; }
    memcpy(alloc->data, stream, n + 1);
    alloc->end = alloc->data + n;
    alloc->size = n + 1;
    alloc->count = 0;
    if (text->alloc_data) {
        HSD_SisLib_Free(text->alloc_data->data);
        HSD_SisLib_Free(text->alloc_data);
    }
    text->alloc_data = alloc;
    text->sis_buffer = alloc->data;
    text->x60 = NULL;
}

void mu_slippi_tournament_notice(HSD_GObj* menu, const char* message)
{
    MainMenuData* data = menu ? menu->user_data : NULL;
    if (data) mu_slippi_menu_description(data->description, message);
}

void mu_slippi_signin_notice(HSD_GObj* menu)
{
    MainMenuData* data;
    u8 query = 2;
    u8 link_line[128] = { 0 };
    unsigned int got = 0;
    if (!menu || !mu_slippi_menus_enabled() || !mu_slippi_yougame_menu()) return;
    data = menu->user_data;
    if (!data) return;
    if (mu_slippi_cmd(0xE7, &query, 1, link_line, sizeof link_line, &got) == 0 &&
        got == sizeof link_line && link_line[0]) {
        link_line[sizeof link_line - 1] = 0;
        mu_slippi_menu_description(data->description, (const char*) link_line);
    } else {
        mu_slippi_menu_description(data->description,
                                  "Sign in to Slippi to play Direct, Teams and Party");
    }
}


/* The panel owns this state, including the replacement title image. The retail
 * four-byte panel data stays first so its animation proc keeps its ABI. */
typedef struct MuSlippiTitlePanel {
    MainMenuPanelData panel;
    HSD_DObj* native_title;
    HSD_JObj* title_joint;
    HSD_TObj* title_texture;
    HSD_ImageDesc* original;
    HSD_ImageDesc image;
    void* allocation;
    HSD_Text* text;
    const char* fallback_reason;
    u32 was_hidden;
    int logged;
} MuSlippiTitlePanel;

typedef struct MuTitleBounds {
    float left, right, bottom, top, z;
} MuTitleBounds;

/* Read the title quad's indexed positions, not a screen-space approximation.
 * Menu archives use unskinned indexed XYZ vertices. Unsupported geometry
 * leaves the native title visible rather than hiding an unrelated object. */
static int mu_slippi_title_bounds(HSD_JObj* joint, HSD_DObj* dobj, MuTitleBounds* b)
{
    HSD_PObj* p = dobj->pobj;
    HSD_VtxDescList* v;
    HSD_VtxDescList* positions = NULL;
    int stride = 0, offset = 0, vertices, i, k;
    const u8* cursor;
    if (!p || p->next || !p->display || !p->verts ||
        (p->flags & (3 << 12))) return 0; /* no envelopes / shape sets */
    for (v = p->verts; v->attr != GX_VA_NULL; ++v) {
        int bytes;
        if (v->attr_type == GX_DIRECT && v->attr <= GX_VA_TEX7MTXIDX) bytes = 1;
        else if (v->attr_type == GX_INDEX8) bytes = 1;
        else if (v->attr_type == GX_INDEX16) bytes = 2;
        else return 0;
        if (v->attr == GX_VA_POS) { positions = v; offset = stride; }
        stride += bytes;
    }
    if (!positions || (positions->attr_type != GX_INDEX8 && positions->attr_type != GX_INDEX16) ||
        positions->comp_type != GX_F32 ||
        positions->comp_cnt != GX_POS_XYZ || DISC_NULL(positions->vertex)) return 0;
    cursor = p->display;
    if ((*cursor & 0xf8) != GX_QUADS && (*cursor & 0xf8) != GX_TRIANGLESTRIP &&
        (*cursor & 0xf8) != GX_TRIANGLEFAN) return 0;
    vertices = (cursor[1] << 8) | cursor[2];
    if (vertices != 4 || 3 + vertices * stride > p->n_display * 32) return 0;
    HSD_JObjSetupMatrix(joint);
    cursor += 3;
    for (i = 0; i < vertices; ++i, cursor += stride) {
        unsigned index = cursor[offset];
        const u8* raw;
        float local[3], world[3];
        if (positions->attr_type == GX_INDEX16) index = (index << 8) | cursor[offset + 1];
        raw = (const u8*) DP(positions->vertex) + index * positions->stride;
        for (k = 0; k < 3; ++k) {
            u32 bits = (u32) raw[k*4] << 24 | (u32) raw[k*4+1] << 16 |
                       (u32) raw[k*4+2] << 8 | raw[k*4+3];
            memcpy(&local[k], &bits, sizeof bits);
        }
        for (k = 0; k < 3; ++k)
            world[k] = joint->mtx[k][0]*local[0] + joint->mtx[k][1]*local[1] +
                       joint->mtx[k][2]*local[2] + joint->mtx[k][3];
        if (i == 0) {
            b->left = b->right = world[0]; b->bottom = b->top = world[1]; b->z = world[2];
        } else {
            if (world[0] < b->left) b->left = world[0];
            if (world[0] > b->right) b->right = world[0];
            if (world[1] < b->bottom) b->bottom = world[1];
            if (world[1] > b->top) b->top = world[1];
        }
    }
    return b->top > b->bottom && b->right - b->left > 3 * (b->top - b->bottom);
}

/* Node 0x29 is the panel's animated heading group (fn_80229BF4).
 * Its rightmost monochrome text quad is the current title; the breadcrumb
 * (1-P Mode) is to its left. Never traverse its siblings (the panel art). */
static void mu_slippi_title_find(HSD_JObj* joint, MuSlippiTitlePanel* state, MuTitleBounds* best)
{
    HSD_DObj* d;
    HSD_JObj* child;
    if (!joint) return;
    if (!(joint->flags & (JOBJ_HIDDEN | JOBJ_PTCL | JOBJ_SPLINE))) {
        for (d = joint->u.dobj; d; d = d->next) {
            HSD_TObj* texture = d->mobj ? d->mobj->tobj : NULL;
            HSD_ImageDesc* image = texture ? texture->imagedesc : NULL;
            MuTitleBounds bounds;
            if (!image || (d->flags & DOBJ_HIDDEN) ||
                (image->format != GX_TF_I4 && image->format != GX_TF_IA4) ||
                !mu_slippi_title_bounds(joint, d, &bounds)) continue;
            if (!state->native_title || bounds.left > best->left) {
                state->native_title = d; state->title_joint = joint; *best = bounds;
            }
        }
    }
    for (child = joint->child; child; child = child->next)
        mu_slippi_title_find(child, state, best);
}

static void mu_slippi_title_close(MuSlippiTitlePanel* state)
{
    if (state->text) {
        HSD_SisLib_803A5CC4(state->text);
        state->text = NULL;
        state->native_title->flags = (state->native_title->flags & ~DOBJ_HIDDEN) | state->was_hidden;
    }
    if (state->title_texture && state->title_texture->imagedesc == &state->image)
        state->title_texture->imagedesc = state->original;
    if (state->allocation) HSD_Free(state->allocation);
    state->allocation = NULL;
    state->title_texture = NULL;
    state->original = NULL;
    state->native_title = NULL;
    state->title_joint = NULL;
}

static void mu_slippi_title_panel_free(void* data)
{
    MuSlippiTitlePanel* state = (MuSlippiTitlePanel*) data;
    mu_slippi_title_close(state);
    HSD_Free(state);
}

void mu_slippi_title_panel_init(HSD_GObj* panel)
{
    MuSlippiTitlePanel* state;
    if (!mu_slippi_menus_enabled() || !mu_slippi_yougame_menu()) return;
    state = (MuSlippiTitlePanel*) HSD_MemAlloc(sizeof *state);
    if (!state) return;
    memset(state, 0, sizeof *state);
    state->panel = *(MainMenuPanelData*) panel->user_data;
    GObj_RemoveUserData(panel);
    GObj_InitUserData(panel, 0, mu_slippi_title_panel_free, state);
}

/* Render for the title's own dimensions/format, retaining its quad, animated
 * material colour and alpha. I4 has one coverage/intensity channel, so its
 * outline uses a darker coverage; IA4 retains a full-alpha black outline. */
static int mu_slippi_title_image(MuSlippiTitlePanel* state, const MuTitleBounds* bounds)
{
    const unsigned char* label = mu_baked_slippi_title;
    HSD_ImageDesc* original = state->title_texture->imagedesc;
    unsigned char* pixels;
    int w = original->width, h = original->height, x, y, left = MU_LABEL_W, right = 0;
    int rows, bytes;
    float scale;
    if (!state->logged) {
        OSReport("online menu: title imagedesc width=%u height=%u format=%u\n",
                 (unsigned) w, (unsigned) h, (unsigned) original->format);
        state->logged = 1;
    }
    state->fallback_reason = "unsupported title dimensions or format";
    if (w < 8 || w > 512 || w % 8 || h < 8 || h > 64 ||
        (original->format != GX_TF_I4 && original->format != GX_TF_IA4)) return 0;
    for (y = 0; y < MU_LABEL_H; ++y) for (x = 0; x < MU_LABEL_W; ++x) {
        int pos = ((y / 4) * (MU_LABEL_W / 8) + x / 8) * 32 + y % 4 * 8 + x % 8;
        if (label[pos] >> 4) { if (x < left) left = x; if (x > right) right = x; }
    }
    /* Keep glyph aspect in world space, left aligned with Online Play. */
    scale = w * (bounds->top - bounds->bottom) / ((bounds->right - bounds->left) * MU_LABEL_H);
    state->fallback_reason = "glyphs do not fit the title quad";
    if (left > right || scale <= 0 || (right - left + 1) * scale > w) return 0;
    rows = original->format == GX_TF_I4 ? 8 : 4;
    bytes = w * ((h + rows - 1) / rows * rows) / (original->format == GX_TF_I4 ? 2 : 1);
    state->allocation = HSD_MemAlloc(bytes + 31);
    state->fallback_reason = "title image allocation failed";
    if (!state->allocation) return 0;
    pixels = (unsigned char*) (((__UINTPTR_TYPE__) state->allocation + 31) & ~(__UINTPTR_TYPE__) 31);
    memset(pixels, 0, bytes);
    for (y = 0; y < h; ++y) for (x = 0; x < w; ++x) {
        int sx = left + (int) (x / scale), sy = y * MU_LABEL_H / h;
        unsigned v = sx <= right ? label[((sy / 4) * (MU_LABEL_W / 8) + sx / 8) * 32 + sy % 4 * 8 + sx % 8] : 0;
        if (original->format == GX_TF_IA4) {
            pixels[((y / 4) * (w / 8) + x / 8) * 32 + y % 4 * 8 + x % 8] = v;
        } else {
            int pos = ((y / 8) * (w / 8) + x / 8) * 64 + y % 8 * 8 + x % 8;
            /* Preserve fractional fill intensity: treating every nonzero nibble
             * as solid alpha thickens and stair-steps the antialiased edge. */
            unsigned ink = ((v >> 4) * (15 + 2 * (v & 15)) + 22) / 45;
            pixels[pos / 2] |= ink << (x % 2 ? 0 : 4);
        }
    }
    state->original = original;
    state->image = *original;
    DISC_SET(state->image.image_ptr, pixels);
    return 1;
}

void mu_slippi_title_panel_update(HSD_GObj* panel, int canvas)
{
    MuSlippiTitlePanel* state;
    MuTitleBounds bounds;
    HSD_JObj* heading = NULL;
    if (panel->user_data_remove_func != mu_slippi_title_panel_free) return;
    state = (MuSlippiTitlePanel*) panel->user_data;
    if (!mu_slippi_title_active()) { mu_slippi_title_close(state); return; }
    if (!state->original && !state->text) {
        lb_80011E24((HSD_JObj*) panel->hsd_obj, &heading, 0x29, -1);
        mu_slippi_title_find(heading, state, &bounds);
        if (!state->native_title) return;
        state->title_texture = state->native_title->mobj->tobj;
        if (!mu_slippi_title_image(state, &bounds)) {
            /* SIS has no slant/outline transform. Use its native glyphs and
             * the title material's colour only when a texture is unsafe. */
            state->text = HSD_SisLib_803A6754(0, canvas);
            if (!state->text) { mu_slippi_title_close(state); return; }
            state->text->default_alignment = 0;
            state->text->default_kerning = 1;
            HSD_SisLib_803A6B98(state->text, 0.0f, 0.0f, "Slippi");
            state->was_hidden = state->native_title->flags & DOBJ_HIDDEN;
            OSReport("online menu: Slippi SIS fallback: %s; native colour, upright (SIS has no slant transform)\n",
                     state->fallback_reason);
        }
    }
    if (state->text) {
        HSD_Material* material = state->native_title->mobj->mat;
        if (!mu_slippi_title_bounds(state->title_joint, state->native_title, &bounds)) {
            mu_slippi_title_close(state); return;
        }
        state->native_title->flags |= DOBJ_HIDDEN;
        if (material) {
            float alpha = material->alpha;
            state->text->text_color = material->diffuse;
            if (alpha < 0) alpha = 0;
            if (alpha > 1) alpha = 1;
            state->text->text_color.a = (u8) (255 * alpha);
        }
        state->text->pos_x = bounds.left;
        state->text->pos_y = -bounds.top;
        state->text->pos_z = bounds.z;
        state->text->font_size.x = state->text->font_size.y = (bounds.top - bounds.bottom) / 32;
        state->text->box_size_x = (bounds.right - bounds.left) / state->text->font_size.x;
        state->text->box_size_y = 32;
        return;
    }
    /* The panel animation may reselect its native image every tick. */
    state->title_texture->imagedesc = &state->image;
}
