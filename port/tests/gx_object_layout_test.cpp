// Platform-free mirrors of the SDK private layouts, with console-width integers.
// gx_object_layout_test.py checks the corresponding declarations and casts in the patch.
#include <cstddef>
#include <cstdint>
#include <cstdio>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using s32 = std::int32_t;
using f32 = float;
enum GXTexFmt { GX_TF_I4 = 0, GX_CTF_Z16L = 0x3C };

struct GXTexObj { u32 dummy[8]; };
struct GXLightObj { u32 dummy[16]; };
struct GXTlutObj { u32 dummy[3]; };
struct GXTexRegion { u32 dummy[4]; };
struct GXTlutRegion { u32 dummy[4]; };
struct GXFifoObj { u8 pad[128]; };

struct TexObjBefore {
    u32 mode0, mode1, image0, image3;
    void* userData;
    GXTexFmt fmt;
    u32 tlutName;
    u16 loadCnt;
    u8 loadFmt, flags;
};
#pragma pack(push, 4)
struct TexObj {
    u32 mode0, mode1, image0, image3;
    void* userData;
    u8 fmt, tlutName, loadFmt, flags;
    u16 loadCnt;
    u8 reserved[2];
};
#pragma pack(pop)
struct LightObj {
    u32 reserved[3], Color;
    f32 a[3], k[3], lpos[3], ldir[3];
};
struct TexRegion {
    u32 image1, image2;
    u16 sizeEven, sizeOdd;
    u8 is32bMipmap, isCached;
};
struct TlutObj { u32 tlut, loadTlut0; u16 numEntries; };
struct TlutRegion { u32 loadTlut1; TlutObj tlutObj; };
struct FifoObj {
    u8* base;
    u8* top;
    u32 size, hiWatermark, loWatermark;
    void* rdPtr;
    void* wrPtr;
    s32 count;
    u8 bind_cpu, bind_gp;
};

static_assert(sizeof(void*) == 8, "this regression targets 64-bit hosts");
static_assert(sizeof(TexObjBefore) == 40);
static_assert(sizeof(GXTexObj) == 32 && sizeof(TexObj) == 32);
static_assert(alignof(TexObj) <= alignof(GXTexObj));
static_assert(offsetof(TexObj, mode0) == 0 && offsetof(TexObj, mode1) == 4);
static_assert(offsetof(TexObj, image0) == 8 && offsetof(TexObj, image3) == 12);
static_assert(offsetof(TexObj, userData) == 16);
static_assert(offsetof(TexObj, fmt) == 24 && offsetof(TexObj, tlutName) == 25);
static_assert(offsetof(TexObj, loadFmt) == 26 && offsetof(TexObj, flags) == 27);
static_assert(offsetof(TexObj, loadCnt) == 28 && offsetof(TexObj, reserved) == 30);
static_assert(sizeof(LightObj) == 64 && sizeof(LightObj) <= sizeof(GXLightObj));
static_assert(sizeof(TlutObj) == 12 && sizeof(TlutObj) <= sizeof(GXTlutObj));
static_assert(sizeof(TexRegion) == 16 && sizeof(TexRegion) <= sizeof(GXTexRegion));
static_assert(sizeof(TlutRegion) == 16 && sizeof(TlutRegion) <= sizeof(GXTlutRegion));
static_assert(sizeof(FifoObj) == 56 && sizeof(FifoObj) <= sizeof(GXFifoObj));

int main()
{
    std::puts("GX public/internal bytes: texture 32/32 (was 40), light 64/64, "
              "TLUT 12/12, texture region 16/16, TLUT region 16/16, FIFO 128/56");
}
