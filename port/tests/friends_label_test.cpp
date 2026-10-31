// Compile the production label adapter with synthetic HSD objects and font pixels.
// No disc files, renderer, host service, or game executable are involved.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "mu_menu_label.h"

static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::abort(); } } while (0)
enum { MENU_KIND_8 = 8, MU_SLP_OPT_FRIENDS = 8, GX_TF_IA4 = 2 };
struct HSD_ImageDesc { void* image_ptr; int width, height, format; };
struct HSD_TObj { HSD_TObj* next; HSD_ImageDesc* imagedesc; HSD_ImageDesc** imagetbl; };
struct HSD_MObj { HSD_TObj* tobj; unsigned color; };
struct HSD_DObj { HSD_DObj* next; HSD_MObj* mobj; };
struct HSD_JObj { union { HSD_DObj* dobj; } u; };
static unsigned char HSD_SisLib_FontAtlas[287 * 512];
static unsigned char HSD_SisLib_8040C680[288 * 2];
static unsigned char HSD_SisLib_8040CB00[288 * 2];
static int menus, offered;
static int mu_slippi_menus_enabled() { return menus; }
static int mu_slippi_friends_available() { return offered; }
#define DISC_SET(slot, value) ((slot) = (value))
#include "friends_label_adapter.inc"

static void font() {
    // Distinct intensities identify each letter independently of the renderer.
    const char* text = "Friends";
    for (int i = 0; i < 7; ++i) {
        int ascii = (text[i] - 0x20) * 2;
        HSD_SisLib_8040C680[ascii] = 0x20;
        HSD_SisLib_8040C680[ascii + 1] = static_cast<unsigned char>(i);
        HSD_SisLib_8040CB00[i * 2] = 5;
        HSD_SisLib_8040CB00[i * 2 + 1] = 13;
        for (int y = 4; y < 28; ++y) for (int x = 4; x < 20; ++x) {
            int p = ((y / 8) * 4 + x / 8) * 64 + y % 8 * 8 + x % 8;
            HSD_SisLib_FontAtlas[i * 512 + p / 2] |= (i + 1) << (x % 2 ? 0 : 4);
        }
    }
}
static unsigned pixel(const unsigned char* p, int x, int y) {
    return p[((y / 4) * 22 + x / 8) * 32 + y % 4 * 8 + x % 8];
}

int main() {
    font();
    unsigned char storage[MU_LABEL_BYTES + 2];
    std::memset(storage, 0xa5, sizeof storage);
    auto render = [&] { return mu_friends_label_render(storage + 1, HSD_SisLib_FontAtlas,
                                  HSD_SisLib_8040C680, HSD_SisLib_8040CB00); };
    CHECK(render());
    CHECK(storage[0] == 0xa5 && storage[sizeof storage - 1] == 0xa5);
    // All seven glyphs, centering, italic offset, IA4 intensity/alpha order,
    // and the full padded tile extent, checked against a separate pixel oracle.
    for (int y = 0; y < 32; ++y) for (int x = 0; x < 176; ++x) {
        unsigned expected = 0;
        int letter = (x - 29 - (29 - y) / 5) / 16;
        if (y >= 2 && y < 28 && x >= 29 + (29 - y) / 5 && letter >= 0 && letter < 7)
            expected = ((letter + 1) << 4) | 15;
        CHECK(pixel(storage + 1, x, y) == expected);
    }
    auto saved = HSD_SisLib_8040C680[('F' - 0x20) * 2];
    HSD_SisLib_8040C680[('F' - 0x20) * 2] = 0x40;
    CHECK(!render());
    HSD_SisLib_8040C680[('F' - 0x20) * 2] = saved;
    HSD_SisLib_8040CB00[0] = 40;
    CHECK(!render());
    HSD_SisLib_8040CB00[0] = 5;
    CHECK(!mu_friends_label_render(nullptr, HSD_SisLib_FontAtlas, HSD_SisLib_8040C680, HSD_SisLib_8040CB00));

    unsigned char update_pixels[] = {0xab, 0xcd};
    HSD_ImageDesc update{update_pixels, 176, 30, GX_TF_IA4}, original = update;
    HSD_ImageDesc border{nullptr, 64, 64, GX_TF_IA4};
    HSD_ImageDesc other_format{nullptr, 176, 30, 6};
    HSD_ImageDesc* table[] = {&update};
    HSD_TObj format{nullptr, &other_format, table};
    HSD_TObj edge{&format, &border, table};
    HSD_TObj texture{&edge, &update, table};
    HSD_MObj material{&texture, 0xffcc00};
    HSD_DObj draw{nullptr, &material}, missing{&draw, nullptr};
    HSD_JObj label{{&missing}};
    for (menus = 0; menus <= 1; ++menus) for (offered = 0; offered <= 1; ++offered)
        for (int kind : {0, 1, 8}) for (int selection = 0; selection < 9; ++selection) {
            texture.imagedesc = &update;
            mu_slippi_friends_label(&label, kind, selection);
            bool friends = menus && offered && kind == MENU_KIND_8 && selection == MU_SLP_OPT_FRIENDS;
            CHECK((texture.imagedesc != &update) == friends);
            CHECK(texture.imagetbl == table && table[0] == &update);
            CHECK(std::memcmp(&update, &original, sizeof update) == 0);
            CHECK(update_pixels[0] == 0xab && update_pixels[1] == 0xcd);
            CHECK(edge.imagedesc == &border && format.imagedesc == &other_format);
            CHECK(material.color == 0xffcc00);
            if (friends) CHECK(std::memcmp(texture.imagedesc->image_ptr, storage + 1, MU_LABEL_BYTES) == 0);
        }
    menus = offered = 1;
    // Creation / hover / unhover reselect the animated image before the hook.
    for (unsigned color : {0xffcc00u, 0u, 0xffcc00u}) {
        material.color = color;
        texture.imagedesc = &update;
        mu_slippi_friends_label(&label, MENU_KIND_8, MU_SLP_OPT_FRIENDS);
        CHECK(texture.imagedesc != &update && material.color == color);
    }
    mu_slippi_friends_label(nullptr, MENU_KIND_8, MU_SLP_OPT_FRIENDS);
    std::printf("friends label: %d/%d checks passed (synthetic font and HSD objects)\n", checks, checks);
}
