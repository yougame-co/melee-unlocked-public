/* Included only by the native mncharsel.c after the CSS private types and refresh helpers.
 * This is a logical setter, never a copy of an HSD object, pointer, or heap allocation. */
#include <mu_css_join.h>
#include <melee/gm/gmvsmelee.h>
int mu_te_shield_restore(int port, int choice);

static void mu_css_float_write(unsigned char* dst, float value)
{
    union { float f; unsigned int u; } v; v.f = value; mu_css_write_le(dst, v.u, 4);
}
static float mu_css_float_read(const unsigned char* src)
{
    union { float f; unsigned int u; } v; v.u = (unsigned int) mu_css_read_le(src,4); return v.f;
}
/* Explicit GameRules members: no structure memcpy (padding and endian are not wire ABI). */
#define MU_CSS_RULE_FIELDS(X) \
    X(0,force_main_menu) X(1,bgm) X(2,mode) X(3,time_limit) X(4,stock_count) \
    X(5,handicap) X(6,damage_ratio) X(7,stage_sel) X(8,stock_time_limit) \
    X(9,friendly_fire) X(10,pause) X(11,score_display) X(12,unk_xc) \
    X(13,xD) X(14,xE) X(15,xF) X(16,unk_x10) X(17,x11) X(18,x12) X(19,x13)
static void mu_css_identity_capture(MuCssPort* out, PlayerInitData* p, int port)
{
    int j;
    out->tag_index = p->nametag;
    out->flags = GetRumbleSettingOfPort(port) ? 1 : 0;
    if (p->nametag < 120 && GetNameText(p->nametag) != NULL) {
        struct NameTagData* tag = GetPersistentNameData(p->nametag);
        out->flags |= 4 | (tag->rumble_enabled ? 2 : 0);
        for (j = 0; j < 8; ++j) out->tag[j] = tag->namedata[j];
    }
    else out->tag_index = 120;
    out->auto_handicap = (u8) *gmMainLib_8015CE44(port,p->nametag);
}
/* Match the cursor clamp and resource tables used by this running game, including m-ex. */
static int mu_css_resources_valid(const MuCssState* s, int report)
{
    int i, j;
    float right = (mu_mex_css_codes & MEX_CSS_CURSOR_RIGHT_29) ? 29.0f : 26.0f;
    for (i = 0; i < 4; ++i) {
        const MuCssPort* p = &s->ports[i];
        int icon = CSS_NONE;
        float x = mu_css_float_read(p->cursor_x);
        if (x > right) {
            if (report) OSReport("css-join invalid: ports[%d].cursor_x=%.9g\n",i,(double)x);
            return 0;
        }
        if (p->fighter == 255) continue;
        for (j = 0; j < CSS_NONE; ++j) if (icons[j].char_kind == p->fighter) { icon = j; break; }
        if (icon == CSS_NONE) {
            if (report) OSReport("css-join invalid: ports[%d].fighter=%u\n",i,(unsigned)p->fighter);
            return 0;
        }
        if (p->costume >= gm_GetNumCostumesForCKind(p->fighter)) {
            if (report) OSReport("css-join invalid: ports[%d].costume=%u\n",i,(unsigned)p->costume);
            return 0;
        }
    }
    return 1;
}
int mu_css_transfer(int op, void* bytes, unsigned size)
{
    MuCssState* s = bytes;
    CSSData* css = mnCharSel_804D6CB0;
    GameRules* rules = gmMainLib_GetGameRules();
    struct GamePrefs* prefs = gmMainLib_GetGamePrefs();
    int i, j, tags[4];
    int live = gm_GetCurrentGameMode() == GM_VS && gm_GetCurrentSceneIndex() == 0 && css != NULL && mu_friends_css_frames >= 2;
    if (!bytes || size != MU_CSS_SIZE) return -1;
    if (op == MU_CSS_IDENTITY) {
        VsModeData* vs = live ? &css->vs : gmVsMelee_GetVsData();
        memzero(s,sizeof(*s)); s->version = MU_CSS_VERSION; s->size = MU_CSS_SIZE;
        for (i = 0; i < 4; ++i) mu_css_identity_capture(&s->ports[i], &vs->start.players[i], i);
        return 0;
    }
    if (!live || mnCharSel_804D6CF5 != 4 || css->match_type != VS_MELEE) return -1;
    for (i = 0; i < 4; ++i) {
        /* A name/rules modal must finish through its own path. No partially open text list
         * can be recreated from a small logical record. Delay capture, never discard it. */
        if (!mnCharSel_804A0BC0[i] || !mnCharSel_804A0BD0[i] ||
            !mnCharSel_803F0E8C[i].data || mnCharSel_803F0E8C[i].data->state) return -1;
    }
    if (op == MU_CSS_CAPTURE || op == MU_CSS_CAPTURE_CHECKED) {
        memzero(s,sizeof(*s)); s->version = MU_CSS_VERSION; s->size = MU_CSS_SIZE;
#define MU_CSS_READ_RULE(at,field) s->rules[at] = rules->field;
        MU_CSS_RULE_FIELDS(MU_CSS_READ_RULE)
#undef MU_CSS_READ_RULE
#if MU_SQUAD
        /* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026) */
        if (saved_stocks >= 0) s->rules[4] = (u8) saved_stocks;
#endif
        mu_css_write_le(s->rules+20,(unsigned int) rules->unk_14,4);
        s->item_frequency = prefs->item_freq;
        mu_css_write_le(s->item_mask,prefs->item_mask,8);
        mu_css_write_le(s->stage_mask,prefs->stage_mask,4);
        s->teams = css->vs.start.rules.is_teams;
        mu_css_write_le(s->stage,css->vs.start.rules.stkind,2);
        s->ready = mnCharSel_804D6CF6 == 1;
        for (i = 0; i < 4; ++i) {
            MuCssPort* out = &s->ports[i];
            CSSDoor* door = &mnCharSel_803F0DFC.doors[i];
            struct CSSCursorData* cursor = mnCharSel_804A0BC0[i];
            struct CSSCharModel* token = mnCharSel_804A0BD0[i];
            PlayerInitData* p = &css->vs.start.players[i];
            out->fighter = door->sel_icon < CSS_NONE ? icons[door->sel_icon].char_kind : 255;
            out->costume = door->costume; out->kind = door->p_kind; out->team = door->team;
            out->cpu = p->cpu_level; out->handicap = p->handicap;
            mu_css_identity_capture(out,p,i);
            if (i < 2) s->shield01[i] = (u8) mu_te_shield_choice(i);
            else s->shield23[i-2] = (u8) mu_te_shield_choice(i);
            out->flags |= (door->selected_since_load ? 8 : 0) |
                (door->is_hold_cpu_slider ? 16 : 0) | (door->is_hold_handicap_slider ? 32 : 0);
            mu_css_float_write(out->cursor_x,cursor->xC); mu_css_float_write(out->cursor_y,cursor->x10);
            mu_css_float_write(out->token_x,token->x8); mu_css_float_write(out->token_y,token->xC);
            mu_css_float_write(out->target_x,token->x10); mu_css_float_write(out->target_y,token->x14);
            out->cursor_state = cursor->x5; out->held_token = cursor->x6; out->cursor_flags = cursor->x7;
            mu_css_write_le(out->cursor_timer,cursor->x8,2); mu_css_write_le(out->cursor_repeat,cursor->xA,2);
            out->token_state = token->x5; out->token_colour = token->x6; out->token_timer = token->x7;
        }
        if (!mu_css_valid(s)) return MU_CSS_NOT_TRANSFERABLE;
        return mu_css_resources_valid(s,op == MU_CSS_CAPTURE_CHECKED) ? 0 : MU_CSS_NOT_TRANSFERABLE;
    }
    if (op != MU_CSS_APPLY || !mu_css_valid(s) || !mu_css_resources_valid(s,1)) return -1;
    /* Validate every resource/index and plan tag slots BEFORE changing any memory. Prefer
     * the host's original slot if bytes agree; otherwise reuse an identical tag or a free slot.
     * A full bank refuses apply rather than overwriting someone's saved name. */
    for (i = 0; i < 4; ++i) {
        const MuCssPort* in = &s->ports[i];
        int icon = CSS_NONE;
        for (j = 0; j < CSS_NONE; ++j) if (icons[j].char_kind == in->fighter) { icon = j; break; }
        if (in->fighter != 255 && (icon == CSS_NONE || in->costume >= gm_GetNumCostumesForCKind(in->fighter))) return -1;
        tags[i] = 120;
        if (!(in->flags & 4)) continue;
        for (j = 0; j < 120; ++j) {
            struct NameTagData* tag = GetPersistentNameData(j);
            int k, equal = tag->rumble_enabled == ((in->flags >> 1) & 1) && tag->x1A2 == in->auto_handicap;
            for (k = 0; k < 8; ++k) equal &= (u8) tag->namedata[k] == in->tag[k];
            if (equal) { tags[i] = j; break; }
        }
        if (tags[i] == 120) {
            for (j = 0; j < 120; ++j) {
                int k, used = GetNameText(j) != NULL;
                for (k = 0; k < i; ++k) used |= tags[k] == j;
                if (!used) { tags[i] = j; break; }
            }
        }
        if (tags[i] == 120) return -1;
    }
#if MU_SQUAD
    /* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026) */
    mnCharSel_SquadJoin();
#endif
#define MU_CSS_WRITE_RULE(at,field) rules->field = s->rules[at];
    MU_CSS_RULE_FIELDS(MU_CSS_WRITE_RULE)
#undef MU_CSS_WRITE_RULE
    rules->unk_14 = (s32) mu_css_read_le(s->rules+20,4);
    prefs->item_freq = s->item_frequency;
    prefs->item_mask = mu_css_read_le(s->item_mask,8);
    prefs->stage_mask = (u32) mu_css_read_le(s->stage_mask,4);
    css->vs.start.rules.is_teams = s->teams;
    css->vs.start.rules.stkind = (u16) mu_css_read_le(s->stage,2);
    /* Menu rule setters write these very GameRules/GamePrefs members. Derive all match rule
     * bitfields/item IDs with the game's converter, rather than encoding compiler bitfields. */
    for (i = 0; i < 4; ++i) {
        const MuCssPort* in = &s->ports[i];
        CSSDoor* door = &mnCharSel_803F0DFC.doors[i];
        struct CSSCursorData* cursor = mnCharSel_804A0BC0[i];
        struct CSSCharModel* token = mnCharSel_804A0BD0[i];
        PlayerInitData* p = &css->vs.start.players[i];
        int icon = CSS_NONE;
        for (j = 0; j < CSS_NONE; ++j) if (icons[j].char_kind == in->fighter) { icon = j; break; }
        {
            const u8 shield = i < 2 ? s->shield01[i] : s->shield23[i-2];
            mu_te_shield_restore(i,shield == 255 ? -1 : shield);
            mu_te_shield_label(i);
        }
        p->ckind = in->fighter == 255 ? ChKind_None : in->fighter;
        p->color = in->costume; p->slot_type = in->kind; p->team = in->team;
        p->cpu_level = in->cpu; p->handicap = in->handicap; p->nametag = tags[i];
        gmMainLib_SetRumbleEnabled(i,in->flags & 1);
        p->rumble_enabled = (in->flags & 4) ? (in->flags >> 1) & 1 : in->flags & 1;
        if (tags[i] < 120) {
            struct NameTagData* tag;
            if (GetNameText(tags[i]) == NULL) InitializePersistentNameData(tags[i]);
            tag = GetPersistentNameData(tags[i]);
            for (j = 0; j < 8; ++j) tag->namedata[j] = in->tag[j];
            /* Retail name entry writes its terminator at len, including byte eight. */
            tag->x1A0 = 0;
            tag->rumble_enabled = (in->flags >> 1) & 1;
        }
        *gmMainLib_8015CE44(i,tags[i]) = in->auto_handicap;
        door->sel_icon = door->sel_icon_prev = icon;
        door->costume = in->costume; door->p_kind = door->p_kind_prev = in->kind; door->team = in->team;
        door->selected_since_load = (in->flags >> 3) & 1;
        door->is_hold_cpu_slider = (in->flags >> 4) & 1;
        door->is_hold_handicap_slider = (in->flags >> 5) & 1;
        door->dooranim_timer = door->slideranim_timer = 0;
        cursor->xC = mu_css_float_read(in->cursor_x); cursor->x10 = mu_css_float_read(in->cursor_y);
        cursor->x5 = in->cursor_state; cursor->x6 = in->held_token; cursor->x7 = in->cursor_flags;
        cursor->x8 = (u16) mu_css_read_le(in->cursor_timer,2); cursor->xA = (u16) mu_css_read_le(in->cursor_repeat,2);
        token->x8 = mu_css_float_read(in->token_x); token->xC = mu_css_float_read(in->token_y);
        token->x10 = mu_css_float_read(in->target_x); token->x14 = mu_css_float_read(in->target_y);
        token->x5 = in->token_state; token->x6 = in->token_colour; token->x7 = in->token_timer;
        mnCharSel_803F0E8C[i].data->use_tag = tags[i] < 120;
        if (tags[i] < 120) {
            HSD_SisLib_803A70A0(mnCharSel_803F0E8C[i].data->text,0,"%s",GetNameText(tags[i]));
            mnCharSel_803F0E8C[i].data->text->default_kerning = 0;
        }
        /* Door/CSP/costume/CPU/handicap and tag refresh, same helper as cursor changes. */
        mnCharSel_8025DB34(i);
        HSD_JObjSetTranslateX(GET_JOBJ(cursor->gobj),cursor->xC);
        HSD_JObjSetTranslateY(GET_JOBJ(cursor->gobj),cursor->x10);
        HSD_JObjSetTranslateX(GET_JOBJ(token->gobj),token->x8);
        HSD_JObjSetTranslateY(GET_JOBJ(token->gobj),token->xC);
        HSD_GObjGXLink_803909D8(token->gobj,
            mnCharSel_804A0BC0[in->token_state ? in->token_state - 1 : 3]->gobj);
    }
    gm_80167BC8(&css->vs);
    mnCharSel_804D6CF6 = s->ready;
    mnCharSel_8025EE8C(css->match_type);
    return 0;
}
#undef MU_CSS_RULE_FIELDS

/* Live pick previews (shim/mu_slippi_css.c): door `door`'s pick as character select shows it now,
 * read only. *ck: the fighter under the coin while the coin is down, else 0xFF (carried, or no
 * fighter). *color: the costume the door's portrait shows while it shows that fighter (X/Y change
 * it at once; players[].color only follows later), else players[port].color. */
void mu_css_pick_preview(int door, int port, unsigned char* ck, unsigned char* color)
{
    PlayerInitData* p;
    CSSDoor* d;
    int held;
    *ck = 0xFF;
    *color = 0;
    if (mnCharSel_804D6CB0 == NULL || door < 0 || door > 3 || port < 0 || port > 3) return;
    p = &mnCharSel_804D6CB0->vs.start.players[port];
    d = &mnCharSel_803F0DFC.doors[door];
    held = mnCharSel_804A0BD0[door] != NULL && mnCharSel_804A0BD0[door]->x5 != 0;
    *color = p->color;
    if (p->ckind >= ChKind_None) return;
    if (d->sel_icon < CSS_NONE && icons[d->sel_icon].char_kind == p->ckind) *color = d->costume;
    if (!held) *ck = (unsigned char) p->ckind;
}
