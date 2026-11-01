// Native Metal spike, translated from gx_d3d12.cpp's explicit draw/PSO path.
// SPDX-License-Identifier: GPL-2.0-or-later
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL.h>
#include <SDL_metal.h>
#include "gx_metal.h"
#include "gx_metal_translate.h"
#include "gx_metal_recipe.h"
#include "embed_mac.h"
#include "embed_mac_pixels.h"
#include "embed_mac_screenshot.h"
#include "../runtime/gx/gx_backend.h"
#include "../runtime/gx/gx_shader.h"
#include "../runtime/gx/gx_texture.h"
#include "../runtime/host/test_env.h"
#include "../runtime/hle/yougame_parity.h"
#ifdef YOUGAME_AGENT_TEST_HOOK
#include "../runtime/hle/yougame_agent_present.h"
#endif
#include <chrono>
#include <cstdarg>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>

// Avoid the Win32 types in the host window header in this Objective-C++ unit.
namespace host {
void log(const char*, ...);
void window_presented(uint64_t);
}
namespace gx {
namespace {
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
struct PhaseTimer {
  double& total;
  Clock::time_point start = Clock::now();
  ~PhaseTimer() { total += milliseconds(start); }
};
std::string hex64(uint64_t value) {
  char text[17]; std::snprintf(text, sizeof text, "%016llx", (unsigned long long)value); return text;
}
// Manual retain/release keeps Objective-C lifetime explicit across compiler callbacks.
template<class T> class Object {
 public:
  Object() = default;
  explicit Object(T value) : value_([value retain]) {}
  Object(const Object& other) : Object(other.value_) {}
  Object(Object&& other) noexcept : value_(other.value_) { other.value_ = nil; }
  ~Object() { [value_ release]; }
  Object& operator=(Object other) { std::swap(value_, other.value_); return *this; }
  T get() const { return value_; }
  explicit operator bool() const { return value_ != nil; }
 private:
  T value_ = nil;
};
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::string error_text(NSError* error) { return error ? error.localizedDescription.UTF8String : "unknown Metal error"; }

struct MetalViewDeleter {
  void operator()(void* view) const { if (view) SDL_Metal_DestroyView(view); }
};
using MetalView = std::unique_ptr<void, MetalViewDeleter>;

struct PipelineKey {
  uint64_t vs = 0, ps = 0;
  uint32_t blend = 0, pixel_format = 0;
  bool operator==(const PipelineKey& other) const {
    return vs == other.vs && ps == other.ps && blend == other.blend && pixel_format == other.pixel_format;
  }
};
struct PipelineHash {
  size_t operator()(const PipelineKey& key) const { return hash_bytes(&key, sizeof key); }
};
using Recipe = metal::Recipe;
struct Pipeline {
  // Only the worker/callback writes these fields, before the release store.
  std::atomic<bool> ready{false};
  Object<id<MTLRenderPipelineState>> state;
  std::string error;
  double build_ms = 0;
  Recipe recipe;
  Clock::time_point queued_at = Clock::now();
  std::atomic<int64_t> first_demand{0};
  std::atomic<uint64_t> last_frame{0}, demand_draws{0};
  double stage_ms = 0, archive_ms = 0, pso_ms = 0;
};
struct Job { PipelineKey key; std::shared_ptr<Pipeline> result; };
struct Stage {
  bool ready = false; // Protected by stages_mutex_, including error/function.
  Object<id<MTLLibrary>> library;
  Object<id<MTLFunction>> function;
  std::string error;
};
int64_t clock_ticks() { return Clock::now().time_since_epoch().count(); }
double elapsed_ticks(int64_t since) {
  return std::chrono::duration<double, std::milli>(Clock::duration(clock_ticks() - since)).count();
}
struct Ring {
  Object<id<MTLBuffer>> buffer;
  size_t used = 0;
  void init(id<MTLDevice> device) {
    id<MTLBuffer> value = [device newBufferWithLength:64u << 20 options:MTLResourceStorageModeShared];
    buffer = Object<id<MTLBuffer>>(value); [value release];
    require(bool(buffer), "Metal upload ring allocation failed");
    // Fault in every shared page at startup, before uploads on the submission path.
    // macOS may still compress/evict these pages later under memory pressure.
    std::memset(buffer.get().contents, 0, buffer.get().length);
  }
  size_t allocate(size_t bytes, size_t alignment = 256) {
    const size_t offset = (used + alignment - 1) & ~(alignment - 1);
    if (offset > buffer.get().length || bytes > buffer.get().length - offset)
      throw std::runtime_error("Metal upload ring exhausted (64 MiB per frame slot)");
    used = offset + bytes;
    return offset;
  }
  size_t upload(const void* data, size_t bytes, size_t alignment = 256) {
    const size_t offset = allocate(bytes, alignment);
    if (bytes) std::memcpy(static_cast<uint8_t*>(buffer.get().contents) + offset, data, bytes);
    return offset;
  }
};
struct Slot {
  Ring ring;
  Object<id<MTLCommandBuffer>> fence;
  Object<id<MTLBuffer>> capture;
  Object<id<MTLTexture>> capture_image;
  std::string capture_path;
  uint32_t capture_width = 0, capture_height = 0, capture_pitch = 0;
  bool capture_retina = false;
  bool readback_pending = false, test_shot = false;
  uint32_t test_present = 0;
  uint64_t test_sequence = 0;
};
#ifdef YOUGAME_AGENT_TEST_HOOK
struct AgentPresent {
  uint32_t present = 0, sim = 0;
  uint8_t major = 0, minor = 0;
  int width = 0, height = 0, window_width = 0, window_height = 0;
  size_t pitch = 0;
  Object<id<MTLBuffer>> pixels;
};
#endif
struct Texture { Object<id<MTLTexture>> image; uint64_t used = 0; };
// Float arrays match the MSL constant structs' float4 alignment without SIMD ABI assumptions.
struct BlitConstants { float rect[4]; uint32_t opaque; };
struct ClearConstants { float color[4]; float depth; };

MTLVertexDescriptor* vertex_layout() {
  MTLVertexDescriptor* layout = [MTLVertexDescriptor vertexDescriptor];
  const MTLVertexFormat formats[] = {MTLVertexFormatFloat3, MTLVertexFormatFloat3,
    MTLVertexFormatUChar4Normalized, MTLVertexFormatUChar4Normalized,
    MTLVertexFormatFloat2, MTLVertexFormatFloat2, MTLVertexFormatFloat2, MTLVertexFormatFloat2,
    MTLVertexFormatFloat2, MTLVertexFormatFloat2, MTLVertexFormatFloat2, MTLVertexFormatFloat2,
    MTLVertexFormatUChar4, MTLVertexFormatUChar4, MTLVertexFormatUChar};
  const NSUInteger offsets[] = {0, 12, 24, 28, 32, 40, 48, 56, 64, 72, 80, 88, 96, 100, 104};
  for (unsigned i = 0; i < 15; ++i) {
    layout.attributes[i].format = formats[i]; layout.attributes[i].offset = offsets[i];
    layout.attributes[i].bufferIndex = 30;
  }
  layout.layouts[30].stride = sizeof(Vertex);
  return layout;
}
void describe_blend(MTLRenderPipelineColorAttachmentDescriptor* rt, uint32_t bm, uint32_t format) {
  static const MTLBlendFactor src[] = {MTLBlendFactorZero, MTLBlendFactorOne, MTLBlendFactorDestinationColor,
    MTLBlendFactorOneMinusDestinationColor, MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha,
    MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusDestinationAlpha};
  static const MTLBlendFactor dst[] = {MTLBlendFactorZero, MTLBlendFactorOne, MTLBlendFactorSourceColor,
    MTLBlendFactorOneMinusSourceColor, MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha,
    MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusDestinationAlpha};
  auto s = src[bits(bm, 8, 3)], d = dst[bits(bm, 5, 3)];
  if (format != 1) {
    if (s == MTLBlendFactorDestinationAlpha) s = MTLBlendFactorOne;
    if (s == MTLBlendFactorOneMinusDestinationAlpha) s = MTLBlendFactorZero;
    if (d == MTLBlendFactorDestinationAlpha) d = MTLBlendFactorOne;
    if (d == MTLBlendFactorOneMinusDestinationAlpha) d = MTLBlendFactorZero;
  }
  rt.blendingEnabled = (bm & 1) != 0;
  rt.sourceRGBBlendFactor = s; rt.destinationRGBBlendFactor = d;
  rt.rgbBlendOperation = (bm & (1 << 11)) ? MTLBlendOperationReverseSubtract : MTLBlendOperationAdd;
  rt.sourceAlphaBlendFactor = MTLBlendFactorOne; rt.destinationAlphaBlendFactor = MTLBlendFactorZero;
  rt.alphaBlendOperation = MTLBlendOperationAdd;
  rt.writeMask = MTLColorWriteMaskNone;
  if (bm & 8) rt.writeMask |= MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
  if (bm & 16) rt.writeMask |= MTLColorWriteMaskAlpha;
}

// The same generic position/color/texture stand-in as D3D12. Four variants are
// created before gameplay; a new GX shader never triggers fallback compilation.
const char* utility_source = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct VC {
 float4 projection[4]; float4 depthparams; float4 viewparams; float4 materials[4];
 float4 lights[40]; float4 texmatrices[24]; float4 transformmatrices[64];
};
struct VI { float3 pos [[attribute(0)]]; float4 color [[attribute(2)]];
 float2 uv [[attribute(4)]]; uint4 matrices [[attribute(12)]]; };
struct VO { float4 pos [[position]]; float4 color; float2 uv; };
vertex VO fallback_vs(VI v [[stage_in]], constant VC& c [[buffer(0)]]) {
 VO o; uint m = v.matrices.x; float4 p(v.pos, 1);
 p = float4(dot(c.transformmatrices[m],p),dot(c.transformmatrices[m+1],p),dot(c.transformmatrices[m+2],p),1);
 o.pos = float4(dot(c.projection[0],p),dot(c.projection[1],p),dot(c.projection[2],p),dot(c.projection[3],p));
 o.pos.z = o.pos.w*c.depthparams.x-o.pos.z*c.depthparams.y;
 o.pos.xy *= sign(c.depthparams.zw*float2(-1,1)); o.pos.xy += o.pos.w*c.depthparams.zw;
 if(o.pos.w==1) o.pos.xy=round(o.pos.xy*c.viewparams.xy)*c.viewparams.zw;
 float4 t(v.uv,1,1); o.uv=float2(dot(t,c.texmatrices[0]),dot(t,c.texmatrices[1])); o.color=v.color; return o;
}
fragment float4 fallback0(VO i [[stage_in]]) { return float4(1); }
fragment float4 fallback1(VO i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
 float4 c=t.sample(s,i.uv); if(c.a<=0) discard_fragment(); return c;
}
fragment float4 fallback2(VO i [[stage_in]]) { if(i.color.a<=0) discard_fragment(); return i.color; }
fragment float4 fallback3(VO i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
 float4 c=i.color*t.sample(s,i.uv); if(c.a<=0) discard_fragment(); return c;
}
struct BO { float4 pos [[position]]; float2 uv; };
struct BC { float4 rect; uint opaque; };
vertex BO blit_vs(uint id [[vertex_id]], constant BC& c [[buffer(0)]]) {
 float2 p=float2((id<<1)&2,id&2); BO o; o.pos=float4(p*float2(2,-2)+float2(-1,1),0,1);
 o.uv=p*c.rect.xy+c.rect.zw; return o;
}
fragment float4 blit_ps(BO i [[stage_in]], constant BC& c [[buffer(0)]], texture2d<float> t [[texture(0)]]) {
 constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);
 float4 v=t.sample(s,i.uv); if(c.opaque) v.a=1; return v;
}
fragment float4 present_ps(BO i [[stage_in]], constant BC& c [[buffer(0)]], texture2d<float> t [[texture(0)]]) {
 // GL's EFB texture retains its default GL_REPEAT, including at the top row.
 // Keep copy filtering clamped; only the player-visible presentation repeats.
 constexpr sampler s(coord::normalized,address::repeat,filter::linear);
 float4 v=t.sample(s,i.uv); if(c.opaque) v.a=1; return v;
}
fragment float4 depth_ps(BO i [[stage_in]], depth2d<float> t [[texture(0)]]) {
 constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::nearest);
 uint z=min(uint((1-t.sample(s,i.uv))*16777216.0),0xffffffu);
 return float4(float3((z>>16)&255,(z>>8)&255,z&255)/255.0,1);
}
struct CC { float4 color; float depth; };
struct CO { float4 color [[color(0)]]; float depth [[depth(any)]]; };
fragment CO clear_ps(constant CC& c [[buffer(0)]]) { CO o; o.color=c.color; o.depth=c.depth; return o; }
)MSL";

// Workers and late Metal callbacks own only this context, never the renderer,
// SDL view, frame slots or host presentation state. A stuck driver may retain
// this compiler context until process exit; destroying the backend stays safe.
struct Compiler {
  Object<id<MTLDevice>> device_;
  std::filesystem::path cache_, seed_cache_;
  Object<id<MTLLibrary>> library(const std::string&);
  void compiler_log(const char*, ...);
  void flush_compiler_log();
  void work(unsigned);
  void save_recipes();
  void build(const Job&, id<MTLBinaryArchive>);
  std::shared_ptr<Stage> function(const std::string&, bool retry_failed);
  std::shared_ptr<Stage> stage(const Recipe&, bool, uint64_t);
  void check_running() const { require(!stopping_.load(), "Metal compiler stopping"); }
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Job> jobs_;
  std::vector<Recipe> recipes_; // Appended under mutex_; read only after stop.
  std::atomic<bool> stopping_{false};
  unsigned active_ = 0; // Protected by mutex_.
  std::condition_variable finished_;
  std::mutex compiler_log_mutex_;
  std::deque<std::string> compiler_logs_;
  // One in-flight compile per stage across all workers, including functions.
  // A stage waiter releases this mutex; translation/Metal calls never hold it.
  std::mutex stages_mutex_;
  std::condition_variable stages_wake_;
  std::unordered_map<std::string, std::shared_ptr<Stage>> stages_;
  std::unordered_map<std::string, std::shared_ptr<Stage>> functions_; // Exact MSL, not a collision-prone hash alias.
  std::atomic<uint32_t> function_builds_{0}, function_shares_{0};
  std::atomic<uint32_t> built_{0}, failed_{0}, cache_hits_{0}, translation_misses_{0};
};

class MetalBackend final : public Backend {
 public:
  MetalBackend(SDL_Window* window, int width, int height, const RenderOptions& options);
  ~MetalBackend() override;
  const RenderOptions* presentation_options() const override { return &options_; }
  void resize(int width, int height) override { width_ = width; height_ = height; }
  void set_skip_present(bool skip) override { skip_ = skip; }
  void submit_frame(const Frame& frame) override { submit_frame(frame, nullptr); }
  void submit_frame(const Frame&, const DrawMatrices*) override;
  void presentation_stats(uint32_t* frames, uint32_t* pipelines, uint32_t* textures) const override {
    if (frames) *frames = presented_;
    if (pipelines) *pipelines = compiler_->built_.load();
    if (textures) *textures = textures_.size() + copies_.size();
  }
  std::string profile_line() const override {
    return "metal: pipelines " + std::to_string(compiler_->built_.load()) + ", fallback draws " + std::to_string(fallback_draws_);
  }
 private:
  void init_utilities();
  Object<id<MTLRenderPipelineState>> startup_pipeline(id<MTLFunction>, id<MTLFunction>, MTLPixelFormat,
                                                      bool, uint32_t = 0x18, bool = false);
  void load_recipes();
  std::shared_ptr<Pipeline> queue_recipe(const Recipe&);
  id<MTLRenderPipelineState> pipeline(const DrawCall&, bool urgent = true);
  void update_widescreen();
  void update_embed_scale();
  id<MTLDepthStencilState> depth_state(uint32_t);
  id<MTLSamplerState> sampler(const TextureRef&);
  id<MTLTexture> texture(const TextureRef&);
  Object<id<MTLTexture>> make_texture(uint32_t, uint32_t, MTLPixelFormat, uint32_t = 1);
  void end_encoder() { if (encoder_) { [encoder_.get() endEncoding]; encoder_ = {}; } }
  id<MTLRenderCommandEncoder> begin_efb();
  void draw(const Frame&, const DrawCall&, const DrawMatrices*, size_t);
  void copy(const EfbCopy&);
  void clear(const EfbCopy&);
  void blit(id<MTLTexture>, id<MTLTexture>, const BlitConstants&, bool = false,
            const MTLViewport* = nullptr, bool = false);
  void log_drawable_size();
  void present(const Frame&, const EfbCopy&);
  void capture(id<MTLTexture>, const Frame&);
  void test_shot(id<MTLTexture>, const Frame&);
  void readback(id<MTLTexture>);
  void flush_capture(Slot&);
#ifdef YOUGAME_AGENT_TEST_HOOK
  void agent_readback(id<MTLTexture>, const Frame&);
  void record_agent_presents();
  std::vector<AgentPresent> agent_presents_;
#endif

  struct SubmitPhases {
    double surface = 0, drawable = 0, readback = 0;
  } phases_;

  SDL_Window* window_;
  MetalView view_;
  CAMetalLayer* layer_ = nil; // retained by view
  CGSize logged_drawable_size_ = CGSizeMake(-1, -1);
  RenderOptions options_;
  int width_, height_, scale_ = 1;
  mac_embed::EmbedScaleState embed_scale_;
  bool skip_ = false;
  std::filesystem::path cache_, seed_cache_;
  Object<id<MTLDevice>> device_;
  Object<id<MTLCommandQueue>> queue_;
  Object<id<MTLTexture>> color_, depth_, white_;
  Object<id<MTLRenderPipelineState>> fallback_[4][3], copy_pso_, depth_copy_pso_, present_pso_, clear_pso_;
  Object<id<MTLRenderPipelineState>> retina_resize_pso_;
  Object<id<MTLDepthStencilState>> clear_depth_, no_depth_;
  std::unordered_map<uint32_t, Object<id<MTLDepthStencilState>>> depths_;
  std::unordered_map<uint64_t, Object<id<MTLSamplerState>>> samplers_;
  std::unordered_map<uint64_t, Texture> textures_;
  std::unordered_map<uint32_t, Texture> copies_;
  std::vector<uint8_t> decode_;
  std::vector<uint32_t> indices_;
  Slot slots_[3];
  uint64_t submitted_ = 0, fallback_draws_ = 0, ring_failures_ = 0;
  uint32_t presented_ = 0;
  std::filesystem::path test_dir_;
  std::vector<uint64_t> test_at_;
  bool test_all_ = false;
  std::ofstream test_hashes_, test_shots_;
  Slot* slot_ = nullptr;
  Object<id<MTLCommandBuffer>> command_;
  Object<id<MTLRenderCommandEncoder>> encoder_;
  std::unordered_map<PipelineKey, std::shared_ptr<Pipeline>, PipelineHash> pipelines_;
  std::shared_ptr<Compiler> compiler_ = std::make_shared<Compiler>();
  bool sync_shaders_ = false;
  std::vector<std::thread> workers_;
  void stop_compiler();
};

Object<id<MTLLibrary>> Compiler::library(const std::string& source) {
  check_running();
  NSError* error = nil;
  MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
  options.fastMathEnabled = NO;
  options.languageVersion = MTLLanguageVersion2_1;
  NSString* text = [[NSString alloc] initWithBytes:source.data() length:source.size() encoding:NSUTF8StringEncoding];
  id<MTLLibrary> result = [device_.get() newLibraryWithSource:text options:options error:&error];
  [text release]; [options release];
  if (!result) throw std::runtime_error("Metal shader: " + error_text(error));
  Object<id<MTLLibrary>> owned(result); [result release]; return owned;
}
Object<id<MTLRenderPipelineState>> MetalBackend::startup_pipeline(id<MTLFunction> vs, id<MTLFunction> ps,
    MTLPixelFormat format, bool depth, uint32_t blend, bool geometry) {
  MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = vs; descriptor.fragmentFunction = ps;
  descriptor.colorAttachments[0].pixelFormat = format;
  describe_blend(descriptor.colorAttachments[0], blend, 1);
  if (depth) descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
  if (geometry) descriptor.vertexDescriptor = vertex_layout();
  NSError* error = nil;
  // Startup only, before a frame exists. The draw path never calls this method.
  id<MTLRenderPipelineState> state = [device_.get() newRenderPipelineStateWithDescriptor:descriptor error:&error];
  [descriptor release];
  if (!state) throw std::runtime_error("Metal startup pipeline: " + error_text(error));
  Object<id<MTLRenderPipelineState>> result(state); [state release]; return result;
}
void MetalBackend::init_utilities() {
  int rw = 0, rh = 0;
  const bool retina = mac_embed::retina_size(window_, &rw, &rh);
  // Added only to the opt-in library; preserve EFB color AND depth when auto resolution follows a display change.
  const std::string retina_source = R"MSL(
fragment CO retina_resize_ps(BO i [[stage_in]], texture2d<float> color [[texture(0)]], depth2d<float> depth [[texture(1)]]) {
  constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::nearest);
  CO o; o.color = color.sample(s, i.uv); o.depth = depth.sample(s, i.uv); return o;
}
)MSL";
  const auto lib = compiler_->library(retina ? std::string(utility_source) + retina_source : std::string(utility_source));
  id<MTLFunction> vertex = [lib.get() newFunctionWithName:@"fallback_vs"];
  for (unsigned variant = 0; variant < 4; ++variant) {
    NSString* name = [NSString stringWithFormat:@"fallback%u", variant];
    id<MTLFunction> pixel = [lib.get() newFunctionWithName:name];
    const uint32_t blends[] = {0x18, 0x18 | 1 | (5 << 5) | (4 << 8), 0x18 | 1 | (1 << 5) | (4 << 8)};
    for (unsigned mode = 0; mode < 3; ++mode)
      fallback_[variant][mode] = startup_pipeline(vertex, pixel, MTLPixelFormatRGBA8Unorm, true, blends[mode], true);
    [pixel release];
  }
  [vertex release]; vertex = [lib.get() newFunctionWithName:@"blit_vs"];
  id<MTLFunction> pixel = [lib.get() newFunctionWithName:@"blit_ps"];
  copy_pso_ = startup_pipeline(vertex, pixel, MTLPixelFormatRGBA8Unorm, false);
  [pixel release]; pixel = [lib.get() newFunctionWithName:@"present_ps"];
  present_pso_ = startup_pipeline(vertex, pixel, MTLPixelFormatBGRA8Unorm, false);
  [pixel release]; pixel = [lib.get() newFunctionWithName:@"depth_ps"];
  depth_copy_pso_ = startup_pipeline(vertex, pixel, MTLPixelFormatRGBA8Unorm, false);
  [pixel release]; pixel = [lib.get() newFunctionWithName:@"clear_ps"];
  clear_pso_ = startup_pipeline(vertex, pixel, MTLPixelFormatRGBA8Unorm, true);
  if (retina) {
    [pixel release]; pixel = [lib.get() newFunctionWithName:@"retina_resize_ps"];
    retina_resize_pso_ = startup_pipeline(vertex, pixel, MTLPixelFormatRGBA8Unorm, true);
  }
  [pixel release]; [vertex release];
  MTLDepthStencilDescriptor* state = [[MTLDepthStencilDescriptor alloc] init];
  state.depthCompareFunction = MTLCompareFunctionAlways; state.depthWriteEnabled = YES;
  id<MTLDepthStencilState> ds = [device_.get() newDepthStencilStateWithDescriptor:state];
  clear_depth_ = Object<id<MTLDepthStencilState>>(ds); [ds release];
  state.depthWriteEnabled = NO; ds = [device_.get() newDepthStencilStateWithDescriptor:state];
  no_depth_ = Object<id<MTLDepthStencilState>>(ds); [ds release]; [state release];
}
MetalBackend::MetalBackend(SDL_Window* window, int width, int height, const RenderOptions& options)
    : window_(window), options_(options), width_(width), height_(height) {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    device_ = Object<id<MTLDevice>>(device); [device release];
    require(bool(device_), "No Metal device");
    compiler_->device_ = device_;
    id<MTLCommandQueue> queue = [device_.get() newCommandQueue];
    queue_ = Object<id<MTLCommandQueue>>(queue); [queue release];
    require(bool(queue_), "No Metal command queue");
    // v3 stores exact shader UIDs; do not reuse possibly mis-keyed v2 MSL.
    cache_ = std::filesystem::path(options.shader_cache.empty() ? "shadercache" : options.shader_cache) / "msl-3";
    std::filesystem::create_directories(cache_);
    const auto pruned = metal::prune_temporary_files(cache_);
    if (pruned) host::log("metal: removed %zu abandoned temporary cache files", pruned);
    compiler_->cache_ = cache_;
    int rw = 0, rh = 0;
    embed_scale_.active = mac_embed::retina_size(window_, &rw, &rh);
    scale_ = embed_scale_.active ? mac_embed::efb_scale(options.efb_scale, rh) :
      std::clamp(options.efb_scale > 0 ? options.efb_scale : (height + EFB_HEIGHT - 1) / EFB_HEIGHT, 1, 6);
    init_utilities();
    for (uint32_t state = 0; state < 32; ++state) depth_state(state);
    for (auto& slot : slots_) slot.ring.init(device_.get());
    indices_.reserve(1 << 20); decode_.reserve(4 << 20);
    color_ = make_texture(EFB_WIDTH * scale_, EFB_HEIGHT * scale_, MTLPixelFormatRGBA8Unorm);
    depth_ = make_texture(EFB_WIDTH * scale_, EFB_HEIGHT * scale_, MTLPixelFormatDepth32Float);
    // SDL can attach a Metal view to the existing hidden SDL window. No GL
    // context is created and no swap call goes through OpenGL in this backend.
    view_.reset(SDL_Metal_CreateView(window_));
    require(view_.get() != nullptr, SDL_GetError());
    mac_embed::attach_metal_view(window_, view_.get());
    layer_ = static_cast<CAMetalLayer*>(SDL_Metal_GetLayer(view_.get()));
    layer_.device = device_.get(); layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer_.displaySyncEnabled = options_.vsync;
    log_drawable_size();
    // Drawable readback is diagnostic only. Normal runs retain Metal's fast
    // framebuffer-only path, including when generic --capture is requested.
    const char* shot = host::test_env::getenv("MELEE_TEST_SHOT");
    layer_.framebufferOnly = shot ? NO : YES;
#ifdef YOUGAME_AGENT_TEST_HOOK
    if (yougame::agent_present::recorder().frames_enabled()) layer_.framebufferOnly = NO;
#endif
    if (shot) {
      const std::string spec(shot); const auto colon = spec.rfind(':');
      if (colon != std::string::npos) {
        test_dir_ = spec.substr(0, colon);
        if (spec.compare(colon + 1, std::string::npos, "all") == 0) {
          test_all_ = true; test_hashes_.open(test_dir_ / "hashes.txt");
        } else {
          for (size_t i = colon + 1; i < spec.size();) {
            size_t j = spec.find(',', i); if (j == std::string::npos) j = spec.size();
            test_at_.push_back(std::strtoull(spec.substr(i, j-i).c_str(), nullptr, 10)); i = j+1;
          }
          test_shots_.open(test_dir_ / "shots.txt", std::ios::app);
        }
      }
    }
    layer_.maximumDrawableCount = 3; layer_.allowsNextDrawableTimeout = YES;
    // Initialize persistent EFB content, including its reversed-Z clear value.
    id<MTLCommandBuffer> command = [queue_.get() commandBuffer];
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color_.get(); pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.depthAttachment.texture = depth_.get(); pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.clearDepth = 0; pass.depthAttachment.storeAction = MTLStoreActionStore;
    [[command renderCommandEncoderWithDescriptor:pass] endEncoding]; [command commit]; [command waitUntilCompleted];
    if (options_.pc_settings) host::log("settings panel: not on Metal yet");
    const char* sync = host::test_env::getenv("MELEE_SYNC_SHADERS");
    sync_shaders_ = sync && std::strcmp(sync, "1") == 0;
    host::log("metal: shader mode: %s", sync_shaders_ ? "synchronous (diagnostic; wait for worker, no fallback)" : "asynchronous (non-blocking fallback)");
    host::log("metal: %s | EFB x%d | 12 startup fallback pipelines; GX pipelines compile on worker", device_.get().name.UTF8String, scale_);
    update_widescreen();
    load_recipes();
    host::log("metal: compiler workers 4; shared stages/functions; demand-priority queue");
    try {
      for (unsigned i = 0; i < 4; ++i) {
        auto context = compiler_;
        { std::lock_guard<std::mutex> lock(context->mutex_); ++context->active_; }
        try {
          workers_.emplace_back([context, i] {
            try { context->work(i); }
            catch (const std::exception& error) { context->compiler_log("metal: compiler worker: %s", error.what()); }
            catch (...) { context->compiler_log("metal: compiler worker: unknown failure"); }
            { std::lock_guard<std::mutex> lock(context->mutex_); --context->active_; }
            context->finished_.notify_all();
          });
        } catch (...) {
          { std::lock_guard<std::mutex> lock(context->mutex_); --context->active_; }
          throw;
        }
      }
    } catch (...) { stop_compiler(); throw; }
  }
}

// host::log writes its newline separately; four worker threads could otherwise
// interleave records with each other and the frame thread. Drain on that thread.
void Compiler::compiler_log(const char* format, ...) {
  char text[4096]; va_list args; va_start(args, format);
  std::vsnprintf(text, sizeof text, format, args); va_end(args);
  std::lock_guard<std::mutex> lock(compiler_log_mutex_); compiler_logs_.emplace_back(text);
}
void Compiler::flush_compiler_log() {
  std::deque<std::string> logs;
  { std::lock_guard<std::mutex> lock(compiler_log_mutex_); logs.swap(compiler_logs_); }
  for (const auto& log : logs) host::log("%s", log.c_str());
}
void Compiler::work(unsigned worker) {
  @autoreleasepool {
    check_running();
    MTLBinaryArchiveDescriptor* descriptor = [[MTLBinaryArchiveDescriptor alloc] init];
    Object<MTLBinaryArchiveDescriptor*> owned_descriptor(descriptor); [descriptor release];
    const auto path = cache_ / ("pipelines-" + std::to_string(worker) + ".metalar");
    std::error_code exists_error;  // non-throwing: a filesystem error here must not terminate the worker thread
    if (std::filesystem::is_regular_file(path, exists_error)) descriptor.url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    check_running();
    NSError* error = nil;
    // 10-31: inside the YouGame app, adding a pipeline to a binary archive crashed in Metal itself
    // (-[MTLCompiler createBinaryArchiveAsRecompileTarget:] null, on the AGX add path) at boot. Archives are only a
    // warm-start cache, so they stay off unless MELEE_METAL_ARCHIVE=1; without one, pipelines compile as usual.
    const char* archive_opt = std::getenv("MELEE_METAL_ARCHIVE");
    id<MTLBinaryArchive> archive = nil;
    if (archive_opt && std::string(archive_opt) == "1") {
      archive = [device_.get() newBinaryArchiveWithDescriptor:descriptor error:&error];
      if (!archive && descriptor.url) {
        check_running();
        descriptor.url = nil; archive = [device_.get() newBinaryArchiveWithDescriptor:descriptor error:&error];
      }
    }
    if (worker == 0) compiler_log("metal: binary archives %s", archive ? "on (MELEE_METAL_ARCHIVE=1)" : "off");
    Object<id<MTLBinaryArchive>> owned_archive(archive); [archive release];
    auto checkpoint = [&] {
      if (!owned_archive || stopping_) return;
      NSError* error = nil;
      const auto pending = cache_ / ("pipelines-" + std::to_string(worker) + "." + std::to_string(getpid()) + "." +
                                      hex64(reinterpret_cast<uintptr_t>(this)) + ".pending.metalar");
      const metal::TemporaryFiles temporary{pending};
      if ([owned_archive.get() serializeToURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:pending.c_str()]] error:&error]) {
        std::error_code ec;
        if (stopping_) return;
        std::filesystem::rename(pending, path, ec);
        if (ec) compiler_log("metal: binary archive save: %s", ec.message().c_str());
      } else compiler_log("metal: binary archive save: %s", error_text(error).c_str());
    };
    unsigned dirty = 0;
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
        if (stopping_) break;
        // Recent live draws outrank untouched seed work. Within the same
        // frame, prefer pipelines blocking the most draws, with FIFO ties.
        auto next = jobs_.begin();
        uint64_t best_frame = 0, best_draws = 0;
        for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
          const auto frame = it->result->last_frame.load(), draws = it->result->demand_draws.load();
          if (frame > best_frame || (frame == best_frame && draws > best_draws)) {
            next = it; best_frame = frame; best_draws = draws;
          }
        }
        job = std::move(*next); jobs_.erase(next);
      }
      @autoreleasepool {
        const auto started = Clock::now();
        try { build(job, owned_archive.get()); }
        catch (const std::exception& error) { job.result->error = error.what(); }
        job.result->build_ms = milliseconds(started);
        const bool success = bool(job.result->state);
        if (success) ++built_; else ++failed_;
        compiler_log("metal: pipeline %s/%s %.3f ms (worker) %s%s", hex64(job.key.vs).c_str(),
                  hex64(job.key.ps).c_str(), job.result->build_ms, success ? "ready" : "failed: ", job.result->error.c_str());
        const auto demand = job.result->first_demand.load();
        compiler_log("metal: pipeline timing %s/%s worker %u queue %.3f build %.3f total %.3f demand %.3f stages %.3f archive %.3f pso %.3f ms",
          hex64(job.key.vs).c_str(), hex64(job.key.ps).c_str(), worker,
          std::chrono::duration<double, std::milli>(started - job.result->queued_at).count(),
          job.result->build_ms, milliseconds(job.result->queued_at), demand ? elapsed_ticks(demand) : -1.,
          job.result->stage_ms, job.result->archive_ms, job.result->pso_ms);
        {
          // Pair publication with the diagnostic wait's mutex to avoid a lost wake.
          std::lock_guard<std::mutex> lock(mutex_);
          job.result->ready.store(true, std::memory_order_release);
        }
        wake_.notify_all();
        bool idle = false;
        { std::lock_guard<std::mutex> lock(mutex_); idle = jobs_.empty(); }
        if (++dirty >= 32 || idle) { checkpoint(); dirty = 0; }
      }
    }
    // Best-effort recipe persistence belongs to the isolated worker too. Slow
    // disk I/O at quit cannot hold the renderer beyond its retirement budget.
    if (worker == 0) {
      try { save_recipes(); }
      catch (const std::exception& error) { compiler_log("metal: recipe save: %s", error.what()); }
    }
  }
}
std::shared_ptr<Stage> Compiler::function(const std::string& source, bool retry_failed) {
  check_running();
  std::shared_ptr<Stage> result;
  {
    std::unique_lock<std::mutex> lock(stages_mutex_);
    auto old = functions_.find(source);
    if (retry_failed && old != functions_.end() && old->second->ready && !old->second->error.empty())
      functions_.erase(old);
    auto [found, inserted] = functions_.emplace(source, nullptr);
    if (!inserted) {
      result = found->second;
      stages_wake_.wait(lock, [&] { return stopping_ || result->ready; });
      check_running();
      if (!result->error.empty()) throw std::runtime_error(result->error);
      ++function_shares_;
      return result;
    }
    found->second = result = std::make_shared<Stage>();
  }
  try {
    result->library = library(source);
    check_running();
    id<MTLFunction> function = [result->library.get() newFunctionWithName:@"gx_main"];
    result->function = Object<id<MTLFunction>>(function); [function release];
    require(bool(result->function), "Metal library missing gx_main");
    ++function_builds_;
  } catch (const std::exception& error) { result->error = error.what(); }
  {
    std::lock_guard<std::mutex> lock(stages_mutex_); result->ready = true;
  }
  stages_wake_.notify_all();
  if (!result->error.empty()) throw std::runtime_error(result->error);
  return result;
}
std::shared_ptr<Stage> Compiler::stage(const Recipe& recipe, bool vertex, uint64_t uid) {
  check_running();
  const auto name = std::string(vertex ? "vs-" : "ps-") + hex64(uid);
  std::shared_ptr<Stage> result;
  {
    std::unique_lock<std::mutex> lock(stages_mutex_);
    auto [found, inserted] = stages_.emplace(name, nullptr);
    if (!inserted) {
      result = found->second;
      stages_wake_.wait(lock, [&] { return stopping_ || result->ready; });
      check_running();
      if (!result->error.empty()) throw std::runtime_error(result->error);
      return result;
    }
    found->second = result = std::make_shared<Stage>();
  }
  const auto started = Clock::now();
  double generation_ms = 0, translation_ms = 0, library_ms = 0;
  const char* source_kind = "cache";
  try {
    auto source = shader_translation::read_text(cache_ / (name + ".metal"));
    if (source.empty() && !seed_cache_.empty()) {
      source = shader_translation::read_text(seed_cache_ / (name + ".metal"));
      if (!source.empty()) source_kind = "seed";
    }
    const bool cached = !source.empty();
    if (cached) ++cache_hits_;
    auto fresh = [&] {
      check_running();
      source_kind = "translate"; ++translation_misses_;
      auto phase = Clock::now();
      const auto hlsl = vertex ? generate_vertex_shader(recipe.vs) : generate_pixel_shader(recipe.ps);
      generation_ms += milliseconds(phase); phase = Clock::now();
      check_running();
      // Force bypass of the bad nonempty cache, without deleting the old file
      // before the replacement translation has succeeded and been renamed.
      source = metal_translation::translate(cache_, name, vertex, hlsl, true, &stopping_);
      translation_ms += milliseconds(phase);
    };
    if (!cached) fresh();
    for (unsigned attempt = 0; ; ++attempt) {
      check_running();
      const auto phase = Clock::now();
      try {
        const auto compiled = function(source, cached && attempt == 1);
        result->library = compiled->library; result->function = compiled->function;
        library_ms += milliseconds(phase);
        break;
      } catch (const std::exception& error) {
        library_ms += milliseconds(phase);
        check_running();
        if (!cached || attempt != 0) throw;
        compiler_log("metal: stage %s rejected %s MSL (%s); retrying fresh translation once",
          name.c_str(), source_kind, error.what());
        fresh();
      }
    }
  } catch (const std::exception& error) { result->error = error.what(); }
  compiler_log("metal: stage %s source %s generation %.3f translation %.3f library %.3f total %.3f ms %s",
    name.c_str(), source_kind, generation_ms, translation_ms, library_ms, milliseconds(started),
    result->error.empty() ? "ready" : "failed");
  {
    std::lock_guard<std::mutex> lock(stages_mutex_); result->ready = true;
  }
  stages_wake_.notify_all();
  if (!result->error.empty()) throw std::runtime_error(result->error);
  return result;
}
void Compiler::build(const Job& job, id<MTLBinaryArchive> archive) {
  check_running();
  const auto started = Clock::now();
  auto vertex = stage(job.result->recipe, true, job.key.vs), pixel = stage(job.result->recipe, false, job.key.ps);
  job.result->stage_ms = milliseconds(started);
  check_running();
  MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  Object<MTLRenderPipelineDescriptor*> owned_descriptor(descriptor); [descriptor release];
  descriptor.vertexFunction = vertex->function.get(); descriptor.fragmentFunction = pixel->function.get();
  descriptor.vertexDescriptor = vertex_layout();
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
  descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
  describe_blend(descriptor.colorAttachments[0], job.key.blend, job.key.pixel_format);
  const auto archive_start = Clock::now();
  check_running();
  if (archive) {
    descriptor.binaryArchives = @[archive];
    NSError* error = nil;
    if (![archive addRenderPipelineFunctionsWithDescriptor:descriptor error:&error])
      compiler_log("metal: archive add: %s", error_text(error).c_str());
  }
  job.result->archive_ms = milliseconds(archive_start);
  check_running();
  const auto pso_start = Clock::now();
  struct Completion {
    std::mutex mutex; std::condition_variable wake; bool done = false;
    Object<id<MTLRenderPipelineState>> pipeline; std::string error;
  };
  auto completion = std::make_shared<Completion>();
  [device_.get() newRenderPipelineStateWithDescriptor:descriptor completionHandler:^(id<MTLRenderPipelineState> state, NSError* error) {
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->pipeline = Object<id<MTLRenderPipelineState>>(state);
    if (!state) completion->error = error_text(error);
    completion->done = true; completion->wake.notify_one();
  }];
  // Only the compiler worker waits for the asynchronous driver callback. The
  // frame thread reads ready with acquire semantics and immediately falls back.
  std::unique_lock<std::mutex> lock(completion->mutex);
  const auto deadline = Clock::now() + std::chrono::seconds(30);
  while (!completion->done) {
    check_running();
    require(Clock::now() < deadline, "Metal async pipeline callback timed out (30 seconds)");
    completion->wake.wait_until(lock, std::min(deadline, Clock::now() + std::chrono::milliseconds(10)));
  }
  check_running();
  job.result->pso_ms = milliseconds(pso_start);
  job.result->state = completion->pipeline;
  job.result->error = completion->error;
}
void MetalBackend::load_recipes() {
  auto load = [&](const std::filesystem::path& path, bool allow_gl_seed) {
    const auto bytes = shader_translation::read_text(path);
    uint64_t magic = 0;
    if (bytes.size() >= sizeof magic) std::memcpy(&magic, bytes.data(), sizeof magic);
    const bool gl_seed = allow_gl_seed && magic == 0x3150524C47505847ull;
    const size_t header_size = (gl_seed ? 3 : 4) * sizeof(uint64_t);
    if (bytes.empty()) return false;
    uint64_t header[4]{};
    if (bytes.size() >= header_size) std::memcpy(header, bytes.data(), header_size);
    const uint64_t count = header[gl_seed ? 1 : 2];
    const uint64_t checksum = header[gl_seed ? 2 : 3];
    using GLRecipe = metal::GLRecipe;
    const size_t record_size = gl_seed ? sizeof(GLRecipe) : sizeof(Recipe);
    if (bytes.size() < header_size || header[0] != (gl_seed ? 0x3150524C47505847ull : metal::recipe_magic) ||
        (!gl_seed && header[1] != sizeof(Recipe)) || count > 16384 ||
        bytes.size() != header_size + count * record_size ||
        hash_bytes(bytes.data() + header_size, bytes.size() - header_size) != checksum) {
      host::log("metal: ignoring incompatible %s recipes", gl_seed ? "GL seed" : "cached"); return false;
    }
    uint64_t rejected = 0;
    for (uint64_t i = 0; i < count; ++i) {
      Recipe recipe{};
      if (gl_seed) {
        GLRecipe legacy{};
        std::memcpy(&legacy, bytes.data() + header_size + i * record_size, sizeof legacy);
        recipe = metal::gl_recipe_inputs(legacy);
        // Validate BEFORE make_recipe: even UID construction uses XF counts.
        if (!metal::valid_recipe(recipe)) { ++rejected; continue; }
        DrawCall draw{}; draw.components = legacy.components; draw.bp = legacy.bp;
        std::memcpy(draw.xf_regs, legacy.xf, sizeof legacy.xf);
        // GL seeds lack texture metadata/count: prewarm ordinary variants only.
        // Live CSS draws get their own exact, widened UID at request time.
        recipe = metal::make_recipe(draw);
      } else {
        std::memcpy(&recipe, bytes.data() + header_size + i * record_size, sizeof recipe);
        if (!metal::valid_recipe(recipe)) { ++rejected; continue; }
      }
      queue_recipe(recipe);
    }
    if (rejected) host::log("metal: skipped %llu invalid recipe records in %s",
      (unsigned long long)rejected, path.c_str());
    host::log("metal: queued %zu %s pipeline recipes before guest startup (%llu source records)",
      pipelines_.size(), gl_seed ? "GL seed" : "cached", (unsigned long long)count);
    return count != rejected;
  };
  const char* root = std::getenv("MELEE_SHADER_SEED");
  if (root && *root) {
    const auto candidate = std::filesystem::path(root) / ("gx-metal-msl-3-s" + std::to_string(scale_));
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate / "recipes.bin", ec)) seed_cache_ = candidate;
  }
  compiler_->seed_cache_ = seed_cache_;
  if (!seed_cache_.empty()) host::log("metal: MSL seed directory %s", seed_cache_.c_str());
  if (load(cache_ / "recipes.bin", false)) return;
  // Like GL, an empty value explicitly disables the bundle seed. This is a
  // normal startup input, not a diagnostic override suppressed by vanilla-only.
  if (!root || !*root) { host::log("metal: GL seed disabled; no cached recipes"); return; }
  if (!seed_cache_.empty() && load(seed_cache_ / "recipes.bin", true)) return;
  const auto seed = std::filesystem::path(root) / ("gx-gl410-v2-s" + std::to_string(scale_)) / "recipes.bin";
  if (!load(seed, true)) host::log("metal: no usable GL seed recipes at %s", seed.c_str());
}
void Compiler::save_recipes() {
  std::vector<Recipe> recipes;
  { std::lock_guard<std::mutex> lock(mutex_); recipes = recipes_; }
  uint64_t header[] = {metal::recipe_magic, sizeof(Recipe), recipes.size(), hash_bytes(recipes.data(), recipes.size() * sizeof(Recipe))};
  const auto pending = cache_ / ("recipes." + std::to_string(getpid()) + "." +
                                 hex64(reinterpret_cast<uintptr_t>(this)) + ".pending.bin");
  const metal::TemporaryFiles temporary{pending};
  std::ofstream file(pending, std::ios::binary);
  file.write(reinterpret_cast<const char*>(header), sizeof header);
  file.write(reinterpret_cast<const char*>(recipes.data()), recipes.size() * sizeof(Recipe));
  file.close();
  if (!file) throw std::runtime_error("Cannot write Metal recipes");
  std::filesystem::rename(pending, cache_ / "recipes.bin");
}
std::shared_ptr<Pipeline> MetalBackend::queue_recipe(const Recipe& recipe) {
  // Depth, cull and scissor are dynamic Metal state, unlike D3D12 PSO state.
  const PipelineKey key{recipe.vs.hash(), recipe.ps.hash(), recipe.blend, recipe.pixel_format};
  auto found = pipelines_.find(key);
  if (found == pipelines_.end()) {
    auto result = std::make_shared<Pipeline>();
    result->recipe = recipe;
    found = pipelines_.emplace(key, result).first;
    {
      std::lock_guard<std::mutex> lock(compiler_->mutex_);
      if (compiler_->stopping_) return result;
      if (compiler_->recipes_.size() < 16384) compiler_->recipes_.push_back(recipe);
      compiler_->jobs_.push_back({key, result});
    }
    compiler_->wake_.notify_one();
  }
  return found->second;
}
id<MTLRenderPipelineState> MetalBackend::pipeline(const DrawCall& draw, bool urgent) {
  const auto recipe = metal::make_recipe(draw);
  const PipelineKey key{recipe.vs.hash(), recipe.ps.hash(), recipe.blend, recipe.pixel_format};
  const auto result = queue_recipe(recipe);
  if (result->ready.load(std::memory_order_acquire) && result->state) return result->state.get();
  if (urgent) {
    int64_t unset = 0; result->first_demand.compare_exchange_strong(unset, clock_ticks());
    result->last_frame.store(submitted_ + 1); result->demand_draws.fetch_add(1);
  }
  if (urgent && sync_shaders_) {
    // Pixel comparisons must exercise the generated material/lighting/TEV
    // shaders, including draws that feed persistent EFB copies. Compilation
    // still belongs to the worker; only this opt-in diagnostic waits for it.
    const auto start = Clock::now();
    std::unique_lock<std::mutex> lock(compiler_->mutex_);
    require(compiler_->wake_.wait_for(lock, std::chrono::seconds(120), [&] {
      return result->ready.load(std::memory_order_acquire);
    }), "Metal diagnostic pipeline wait timed out (120 seconds)");
    if (!result->state) throw std::runtime_error("Metal diagnostic pipeline failed: " + result->error);
    host::log("metal: diagnostic pipeline wait %s/%s %.3f ms", hex64(key.vs).c_str(),
              hex64(key.ps).c_str(), milliseconds(start));
    return result->state.get();
  }
  ++fallback_draws_;
  const bool textured = (draw.xf_regs[0x3F] & 15) && (draw.components & VB_HAS_UV0);
  const unsigned variant = unsigned(textured) | ((draw.components & VB_HAS_COL0) ? 2u : 0u);
  // Bound the fallback PSO set at startup. Unusual blends temporarily use alpha
  // over; their exact blend/write mask arrives with the real pipeline.
  const unsigned blend = !(key.blend & 1) ? 0 : bits(key.blend, 5, 3) == 1 ? 2 : 1;
  return fallback_[variant][blend].get();
}
id<MTLDepthStencilState> MetalBackend::depth_state(uint32_t zm) {
  zm &= 0x1f;
  if (auto found = depths_.find(zm); found != depths_.end()) return found->second.get();
  static const MTLCompareFunction compare[] = {MTLCompareFunctionNever, MTLCompareFunctionGreater,
    MTLCompareFunctionEqual, MTLCompareFunctionGreaterEqual, MTLCompareFunctionLess,
    MTLCompareFunctionNotEqual, MTLCompareFunctionLessEqual, MTLCompareFunctionAlways};
  MTLDepthStencilDescriptor* descriptor = [[MTLDepthStencilDescriptor alloc] init];
  descriptor.depthCompareFunction = (zm & 1) ? compare[bits(zm, 1, 3)] : MTLCompareFunctionAlways;
  descriptor.depthWriteEnabled = (zm & 1) && (zm & 16);
  id<MTLDepthStencilState> state = [device_.get() newDepthStencilStateWithDescriptor:descriptor];
  depths_.emplace(zm, Object<id<MTLDepthStencilState>>(state)); [state release]; [descriptor release];
  return depths_.at(zm).get();
}
Object<id<MTLTexture>> MetalBackend::make_texture(uint32_t width, uint32_t height, MTLPixelFormat format, uint32_t levels) {
  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:levels > 1];
  descriptor.mipmapLevelCount = levels;
  descriptor.storageMode = MTLStorageModePrivate;
  descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
  id<MTLTexture> texture = [device_.get() newTextureWithDescriptor:descriptor];
  require(texture != nil, "Metal texture allocation failed");
  Object<id<MTLTexture>> owned(texture); [texture release]; return owned;
}
id<MTLSamplerState> MetalBackend::sampler(const TextureRef& texture) {
  const uint32_t m0 = texture.mode0, m1 = texture.mode1;
  const uint64_t key = uint64_t(m0) | (uint64_t(m1) << 32);
  if (auto found = samplers_.find(key); found != samplers_.end()) return found->second.get();
  MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
  static const MTLSamplerAddressMode wrap[] = {MTLSamplerAddressModeClampToEdge, MTLSamplerAddressModeRepeat,
    MTLSamplerAddressModeMirrorRepeat, MTLSamplerAddressModeRepeat};
  descriptor.sAddressMode = wrap[bits(m0, 0, 2)]; descriptor.tAddressMode = wrap[bits(m0, 2, 2)];
  descriptor.magFilter = bits(m0, 4, 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
  const uint32_t min = bits(m0, 5, 3), mip = min & 3;
  descriptor.minFilter = (min & 4) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
  descriptor.mipFilter = !mip ? MTLSamplerMipFilterNotMipmapped : mip == 2 ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
  descriptor.lodMinClamp = bits(m1, 0, 8) / 16.0f;
  descriptor.lodMaxClamp = mip ? bits(m1, 8, 8) / 16.0f : 0;
  // GL/D3D12 enable anisotropy only when both min and mag filters are linear.
  descriptor.maxAnisotropy = (min & 4) && bits(m0, 4, 1) ? std::clamp(options_.anisotropy, 1, 16) : 1;
  id<MTLSamplerState> state = [device_.get() newSamplerStateWithDescriptor:descriptor];
  require(state != nil, "Metal sampler allocation failed");
  samplers_.emplace(key, Object<id<MTLSamplerState>>(state)); [state release]; [descriptor release];
  return samplers_.at(key).get();
}
id<MTLTexture> MetalBackend::texture(const TextureRef& texture) {
  if (texture.used) {
    if (auto found = copies_.find(texture.addr & 0x3fffffffu); found != copies_.end()) {
      found->second.used = submitted_; return found->second.image.get();
    }
  }
  if (!texture.used || !texture.data || !texture.width || !texture.height) {
    if (!white_) {
      white_ = make_texture(1, 1, MTLPixelFormatRGBA8Unorm);
      const uint32_t white = 0xffffffff;
      const auto offset = slot_->ring.upload(&white, sizeof white);
      end_encoder(); id<MTLBlitCommandEncoder> encoder = [command_.get() blitCommandEncoder];
      [encoder copyFromBuffer:slot_->ring.buffer.get() sourceOffset:offset sourceBytesPerRow:256 sourceBytesPerImage:256
                  sourceSize:MTLSizeMake(1, 1, 1) toTexture:white_.get() destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
      [encoder endEncoding];
    }
    return white_.get();
  }
  const uint32_t metadata[] = {texture.width, texture.height, texture.format, texture.mip_levels, texture.tlut_format};
  const uint64_t key = texture.data->hash ^ hash_bytes(metadata, sizeof metadata);
  if (auto found = textures_.find(key); found != textures_.end()) { found->second.used = submitted_; return found->second.image.get(); }
  auto image = make_texture(texture.width, texture.height, MTLPixelFormatRGBA8Unorm, texture.mip_levels);
  size_t source_offset = 0;
  end_encoder(); id<MTLBlitCommandEncoder> encoder = [command_.get() blitCommandEncoder];
  try {
    for (uint32_t level = 0; level < texture.mip_levels; ++level) {
      const uint32_t w = std::max(1u, texture.width >> level), h = std::max(1u, texture.height >> level);
      const size_t bytes = texture_level_bytes(w, h, texture.format);
      if (source_offset + bytes > texture.data->image.size()) throw std::runtime_error("truncated GX texture snapshot");
      decode_texture(texture.data->image.data() + source_offset, w, h, texture.format,
        texture.data->palette.empty() ? nullptr : texture.data->palette.data(), texture.tlut_format, decode_);
      source_offset += bytes;
      const size_t pitch = (size_t(w) * 4 + 255) & ~size_t(255);
      const auto offset = slot_->ring.allocate(pitch * h);
      auto* output = static_cast<uint8_t*>(slot_->ring.buffer.get().contents) + offset;
      for (uint32_t y = 0; y < h; ++y) std::memcpy(output + y * pitch, decode_.data() + size_t(y) * w * 4, size_t(w) * 4);
      [encoder copyFromBuffer:slot_->ring.buffer.get() sourceOffset:offset sourceBytesPerRow:pitch sourceBytesPerImage:pitch * h
                  sourceSize:MTLSizeMake(w, h, 1) toTexture:image.get() destinationSlice:0 destinationLevel:level destinationOrigin:MTLOriginMake(0, 0, 0)];
    }
  } catch (...) { [encoder endEncoding]; throw; }
  [encoder endEncoding];
  textures_.emplace(key, Texture{image, submitted_}); return image.get();
}

id<MTLRenderCommandEncoder> MetalBackend::begin_efb() {
  if (encoder_) return encoder_.get();
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = color_.get();
  pass.colorAttachments[0].loadAction = MTLLoadActionLoad; pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.depthAttachment.texture = depth_.get();
  pass.depthAttachment.loadAction = MTLLoadActionLoad; pass.depthAttachment.storeAction = MTLStoreActionStore;
  encoder_ = Object<id<MTLRenderCommandEncoder>>([command_.get() renderCommandEncoderWithDescriptor:pass]);
  require(bool(encoder_), "Metal EFB encoder failed");
  return encoder_.get();
}
void MetalBackend::draw(const Frame& frame, const DrawCall& draw, const DrawMatrices* overrides, size_t geometry) {
  if (size_t(draw.first_vertex) + draw.vertex_count > frame.vertices.size()) return;
  indices_.clear();
  DrawTopology topology = DrawTopology::Unsupported;
  const bool replaced = overrides && overrides->vertices;
  auto append = [&](uint32_t primitive, uint32_t count, uint32_t base) {
    auto next = append_segment_indices(indices_, primitive, count, replaced ? base - draw.first_vertex : base);
    if (topology != DrawTopology::Unsupported && next != topology)
      throw std::runtime_error("mixed primitive topology in Metal draw");
    topology = next;
  };
  if (draw.segment_count) {
    if (size_t(draw.first_segment) + draw.segment_count > frame.segments.size()) return;
    for (uint32_t i = 0; i < draw.segment_count; ++i) {
      const auto& segment = frame.segments[draw.first_segment + i];
      if (segment.first_vertex < draw.first_vertex || size_t(segment.first_vertex) + segment.vertex_count > size_t(draw.first_vertex) + draw.vertex_count) return;
      append(segment.primitive, segment.vertex_count, segment.first_vertex);
    }
  } else append(draw.primitive, draw.vertex_count, draw.first_vertex);
  if (indices_.empty()) return;
  if (replaced) geometry = slot_->ring.upload(overrides->vertices, size_t(draw.vertex_count) * sizeof(Vertex));
  const auto index_offset = slot_->ring.upload(indices_.data(), indices_.size() * sizeof(uint32_t), 4);
  VSConstants vs{}; PSConstants ps{};
  fill_vs_constants(draw, vs, scale_, overrides); fill_ps_constants(draw, ps, scale_);
  const auto vs_offset = slot_->ring.upload(&vs, vs_constants_bytes(draw, false));
  const auto ps_offset = slot_->ring.upload(&ps, sizeof ps);
  // Metal has no sampler descriptor LOD bias; SampleBias in the translated
  // shader adds the same signed GX 1/32-level bias as GL_TEXTURE_LOD_BIAS.
  float lod_bias[8];
  for (unsigned i = 0; i < 8; ++i) lod_bias[i] = sbits(draw.textures[i].mode0, 9, 8) / 32.0f;
  const auto bias_offset = slot_->ring.upload(lod_bias, sizeof lod_bias);
  const auto pipeline_state = pipeline(draw);
  id<MTLTexture> images[8]; id<MTLSamplerState> samplers[8];
  // Texture misses insert blit encoders. Resolve them before opening/restoring
  // the render encoder, preserving the D3D12 command stream's copy/draw order.
  for (unsigned i = 0; i < 8; ++i) { images[i] = texture(draw.textures[i]); samplers[i] = sampler(draw.textures[i]); }
  const float* vp = reinterpret_cast<const float*>(&draw.xf_regs[0x1a]);
  float x = (vp[3] - vp[0] - 342) * scale_, y = (vp[4] + vp[1] - 342) * scale_;
  float w = 2 * vp[0] * scale_, h = -2 * vp[1] * scale_;
  if (w < 0) { x += w; w = -w; } if (h < 0) { y += h; h = -h; }
  float near = std::clamp(1 - vp[5] / 16777216.0f, 0.0f, 1.0f);
  float far = std::clamp(1 - (vp[5] - vp[2]) / 16777216.0f, 0.0f, 1.0f);
  if (far < near) std::swap(near, far);
  if (!std::isfinite(x + y + w + h + near + far)) return;
  const uint32_t tl = draw.bp.reg[BP_SCISSORTL], br = draw.bp.reg[BP_SCISSORBR], so = draw.bp.reg[BP_SCISSOROFFSET];
  const int xoff = int(bits(so, 0, 10)) * 2 - 342, yoff = int(bits(so, 10, 10)) * 2 - 342;
  const int left = std::clamp(int(bits(tl, 12, 12)) - xoff - 342, 0, EFB_WIDTH) * scale_;
  const int top = std::clamp(int(bits(tl, 0, 12)) - yoff - 342, 0, EFB_HEIGHT) * scale_;
  const int right = std::clamp(int(bits(br, 12, 12)) - xoff - 341, 0, EFB_WIDTH) * scale_;
  const int bottom = std::clamp(int(bits(br, 0, 12)) - yoff - 341, 0, EFB_HEIGHT) * scale_;
  if (right <= left || bottom <= top) return;
  const uint32_t cull = draw.bp.cullmode();
  if (cull == 3) return; // GX_CULL_ALL
  auto encoder = begin_efb();
  [encoder setRenderPipelineState:pipeline_state];
  [encoder setDepthStencilState:depth_state(draw.bp.zmode())];
  [encoder setFrontFacingWinding:MTLWindingClockwise];
  [encoder setCullMode:cull == 1 ? MTLCullModeBack : cull == 2 ? MTLCullModeFront : MTLCullModeNone];
  [encoder setViewport:MTLViewport{x, y, std::max(w, 1.0f), std::max(h, 1.0f), near, far}];
  [encoder setScissorRect:MTLScissorRect{NSUInteger(left), NSUInteger(top), NSUInteger(right-left), NSUInteger(bottom-top)}];
  [encoder setVertexBuffer:slot_->ring.buffer.get() offset:geometry atIndex:30];
  [encoder setVertexBuffer:slot_->ring.buffer.get() offset:vs_offset atIndex:0];
  [encoder setFragmentBuffer:slot_->ring.buffer.get() offset:ps_offset atIndex:1];
  [encoder setFragmentBuffer:slot_->ring.buffer.get() offset:bias_offset atIndex:2];
  [encoder setFragmentTextures:images withRange:NSMakeRange(0, 8)];
  [encoder setFragmentSamplerStates:samplers withRange:NSMakeRange(0, 8)];
  [encoder drawIndexedPrimitives:topology == DrawTopology::Lines ? MTLPrimitiveTypeLine : MTLPrimitiveTypeTriangle
    indexCount:indices_.size() indexType:MTLIndexTypeUInt32 indexBuffer:slot_->ring.buffer.get() indexBufferOffset:index_offset];
}
void MetalBackend::blit(id<MTLTexture> source, id<MTLTexture> destination, const BlitConstants& constants,
                         bool depth, const MTLViewport* viewport, bool present) {
  end_encoder();
  const auto offset = slot_->ring.upload(&constants, sizeof constants);
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = destination;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear; pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
  id<MTLRenderCommandEncoder> encoder = [command_.get() renderCommandEncoderWithDescriptor:pass];
  [encoder setRenderPipelineState:present ? present_pso_.get() : depth ? depth_copy_pso_.get() : copy_pso_.get()];
  [encoder setDepthStencilState:no_depth_.get()];
  [encoder setCullMode:MTLCullModeNone];
  const MTLViewport full{0, 0, double(destination.width), double(destination.height), 0, 1};
  [encoder setViewport:viewport ? *viewport : full];
  if (viewport) {
    // Metal's guard-band rasterization does not bound an oversized fullscreen
    // triangle to the viewport. GL clips it; constrain the integer picture here.
    [encoder setScissorRect:MTLScissorRect{NSUInteger(viewport->originX), NSUInteger(viewport->originY),
      NSUInteger(viewport->width), NSUInteger(viewport->height)}];
  }
  [encoder setVertexBuffer:slot_->ring.buffer.get() offset:offset atIndex:0];
  [encoder setFragmentBuffer:slot_->ring.buffer.get() offset:offset atIndex:0];
  [encoder setFragmentTexture:source atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [encoder endEncoding];
}
void MetalBackend::copy(const EfbCopy& copy) {
  if (!copy.src_w || !copy.src_h) return;
  const uint32_t w = std::max(1u, copy.src_w / (copy.half_scale ? 2 : 1)) * scale_;
  const uint32_t h = std::max(1u, copy.src_h / (copy.half_scale ? 2 : 1)) * scale_;
  auto& destination = copies_[copy.dest_addr & 0x3fffffffu];
  if (!destination.image || destination.image.get().width != w || destination.image.get().height != h)
    destination.image = make_texture(w, h, MTLPixelFormatRGBA8Unorm);
  destination.used = submitted_;
  BlitConstants constants{{copy.src_w / float(EFB_WIDTH), copy.src_h / float(EFB_HEIGHT),
    copy.src_x / float(EFB_WIDTH), copy.src_y / float(EFB_HEIGHT)}, !copy.efb_alpha ? 1u : 0u};
  // The conversion matches D3D12: half-scale filtering, opaque RGB EFB,
  // and 24-bit depth packed into RGB. Copy-format quantization is not modeled.
  blit(copy.is_depth ? depth_.get() : color_.get(), destination.image.get(), constants, copy.is_depth);
}
void MetalBackend::clear(const EfbCopy& copy) {
  const uint32_t left = std::min(copy.src_x, uint32_t(EFB_WIDTH)) * scale_;
  const uint32_t top = std::min(copy.src_y, uint32_t(EFB_HEIGHT)) * scale_;
  const uint32_t right = std::min(copy.src_x + copy.src_w, uint32_t(EFB_WIDTH)) * scale_;
  const uint32_t bottom = std::min(copy.src_y + copy.src_h, uint32_t(EFB_HEIGHT)) * scale_;
  if (right <= left || bottom <= top) return;
  ClearConstants constants{{((copy.clear_color >> 16) & 255) / 255.0f, ((copy.clear_color >> 8) & 255) / 255.0f,
    (copy.clear_color & 255) / 255.0f, (copy.clear_color >> 24) / 255.0f}, 1 - copy.clear_z / 16777215.0f};
  const BlitConstants full{{1, 1, 0, 0}, 0};
  const auto vertex = slot_->ring.upload(&full, sizeof full), fragment = slot_->ring.upload(&constants, sizeof constants);
  auto encoder = begin_efb();
  [encoder setRenderPipelineState:clear_pso_.get()]; [encoder setDepthStencilState:clear_depth_.get()];
  [encoder setCullMode:MTLCullModeNone];
  [encoder setViewport:MTLViewport{0, 0, double(color_.get().width), double(color_.get().height), 0, 1}];
  [encoder setScissorRect:MTLScissorRect{left, top, right-left, bottom-top}];
  [encoder setVertexBuffer:slot_->ring.buffer.get() offset:vertex atIndex:0];
  [encoder setFragmentBuffer:slot_->ring.buffer.get() offset:fragment atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}
void MetalBackend::log_drawable_size() {
  const CGSize size = layer_.drawableSize;
  if (CGSizeEqualToSize(size, logged_drawable_size_)) return;
  logged_drawable_size_ = size;
  host::log("metal: drawable %.0fx%.0f", size.width, size.height);
}
void MetalBackend::present(const Frame& frame, const EfbCopy& copy) {
  end_encoder();
  int w = 0, h = 0;
  {
    PhaseTimer timing{phases_.surface};
    // Embedded dimensions follow the picture snapshot in both Retina modes; SDL's layer can lag a view-only resize.
    const bool embedded = mac_embed::drawable_size(window_, &w, &h);
    if (!embedded) SDL_Metal_GetDrawableSize(window_, &w, &h);
    if (w > 0 && h > 0) {
      // The adapter owns embedded layer sizing on the main thread, including the parent-gone transition to 1x.
      if (!embedded) layer_.drawableSize = CGSizeMake(w, h);
      layer_.displaySyncEnabled = options_.vsync;
    }
  }
  log_drawable_size();
  const uint64_t screenshot = mac_embed::screenshot_take_requested() ? mac_embed::screenshot_take() : 0;
  if (w <= 0 || h <= 0) { mac_embed::screenshot_failed(screenshot); return; }
  BOOL framebuffer_only = YES;
  if (screenshot) { framebuffer_only = layer_.framebufferOnly; layer_.framebufferOnly = NO; }
  const auto start = Clock::now();
  id<CAMetalDrawable> drawable = [layer_ nextDrawable];
  if (screenshot) layer_.framebufferOnly = framebuffer_only;
  const double drawable_ms = milliseconds(start);
  phases_.drawable += drawable_ms;
  if (!drawable) { mac_embed::screenshot_failed(screenshot); host::log("metal: nextDrawable unavailable at simulation %llu", (unsigned long long)frame.sequence); return; }
  // Use GL's lround fit and floor centering, converting its bottom-left Y to
  // Metal's top-left Y. Fractional viewports shift sampling and leak into bars.
  const double aspect = presented_aspect(options_, w, h, false);
  int output_w = w, output_h = std::lround(w / aspect);
  if (output_h > h) { output_h = h; output_w = std::lround(h * aspect); }
  int rw = 0, rh = 0;
  if (mac_embed::retina_size(window_, &rw, &rh)) {
    const auto box = mac_embed::letterbox(w, h, aspect); output_w = box.w; output_h = box.h;
  }
  const int left = (w-output_w)/2, bottom = (h-output_h)/2;
  const MTLViewport viewport{double(left), double(h-bottom-output_h), double(output_w), double(output_h), 0, 1};
  BlitConstants constants{{copy.src_w / float(EFB_WIDTH), copy.src_h / float(EFB_HEIGHT),
    copy.src_x / float(EFB_WIDTH), copy.src_y / float(EFB_HEIGHT)}, 1};
  blit(color_.get(), drawable.texture, constants, false, &viewport, true);
  ++presented_;
  // Generic captures without MELEE_TEST_SHOT render the same presentation pass
  // into a private texture; a framebuffer-only drawable cannot be a blit source.
  const bool generic = !options_.capture_path.empty() &&
    ((options_.capture_every && presented_ % options_.capture_every == 0) ||
     (options_.capture_frame && presented_ == options_.capture_frame));
  if (generic && layer_.framebufferOnly) {
    if (!slot_->capture_image || slot_->capture_image.get().width != drawable.texture.width ||
        slot_->capture_image.get().height != drawable.texture.height)
      slot_->capture_image = make_texture(w, h, MTLPixelFormatBGRA8Unorm);
    blit(color_.get(), slot_->capture_image.get(), constants, false, &viewport, true);
    capture(slot_->capture_image.get(), frame);
  } else if (generic) capture(drawable.texture, frame);
  test_shot(drawable.texture, frame);
#ifdef YOUGAME_AGENT_TEST_HOOK
  agent_readback(drawable.texture, frame);
#endif
  if (screenshot) {
    // Read the finished drawable (bars and presentation included), with no GPU wait.
    const int sw = int(drawable.texture.width), sh = int(drawable.texture.height);
    const size_t pitch = (size_t(sw) * 4 + 255) & ~size_t(255);
    id<MTLBuffer> buffer = mac_embed::shot::source_ok(sw, sh) ?
      [device_.get() newBufferWithLength:pitch * sh options:MTLResourceStorageModeShared] : nil;
    if (!buffer) mac_embed::screenshot_failed(screenshot);
    else {
      std::shared_ptr<const void> owner((const void*)buffer, [](const void* p) { [(id)p release]; });
      id<MTLBlitCommandEncoder> encoder = [command_.get() blitCommandEncoder];
      if (!encoder) mac_embed::screenshot_failed(screenshot);
      else {
        [encoder copyFromTexture:drawable.texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
          sourceSize:MTLSizeMake(sw, sh, 1) toBuffer:buffer destinationOffset:0
          destinationBytesPerRow:pitch destinationBytesPerImage:pitch * sh];
        [encoder endEncoding];
        [command_.get() addCompletedHandler:^(id<MTLCommandBuffer> completed) {
          if (completed.status != MTLCommandBufferStatusCompleted) mac_embed::screenshot_failed(screenshot);
          else mac_embed::screenshot_submit(screenshot, sw, sh, pitch, true, false, owner,
                                            static_cast<const uint8_t*>(buffer.contents));
        }];
      }
    }
  }
  // Scheduling a drawable is not a successful present. Release the app's loading
  // cover only when Metal reports it on screen, using GL's real-frame rule.
  const bool real_frame = frame.test_fill < 0;
  [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
    if (presented.presentedTime > 0)
      yougame::parity::signals.presented(real_frame);
  }];
  [command_.get() presentDrawable:drawable];
  host::log("metal: present %u simulation %llu drawable %.3f ms", presented_, (unsigned long long)frame.sequence, drawable_ms);
}
void MetalBackend::capture(id<MTLTexture> image, const Frame& frame) {
  const auto& base = options_.capture_path;
  if (base.empty()) return;
  const bool periodic = options_.capture_every && presented_ % options_.capture_every == 0;
  const bool exact = options_.capture_frame && presented_ == options_.capture_frame;
  // For the spike's measurements use --capture-every or --capture-frame.
  if (!periodic && !exact) return;
  std::string path = base;
  if (periodic) {
    if (path.size() > 4 && path.compare(path.size()-4, 4, ".ppm") == 0) path.resize(path.size()-4);
    char suffix[48]; std::snprintf(suffix, sizeof suffix, "_%05u.ppm", presented_); path += suffix;
  }
  slot_->capture_path = path;
  readback(image);
  host::log("metal: capture %s simulation %llu scene %u:%u", path.c_str(), (unsigned long long)frame.sequence, frame.scene_major, frame.scene_minor);
}
void MetalBackend::test_shot(id<MTLTexture> image, const Frame& frame) {
  // GL's legacy test-shot numbering is zero-based, unlike --capture-frame.
  const uint32_t n = presented_ - 1;
  if (!test_all_ && std::find(test_at_.begin(), test_at_.end(), n) == test_at_.end()) return;
  slot_->test_shot = true; slot_->test_present = n; slot_->test_sequence = frame.sequence;
  readback(image);
}
void MetalBackend::readback(id<MTLTexture> image) {
  if (slot_->readback_pending) return; // Both capture hooks share this finished picture.
  PhaseTimer timing{phases_.readback};
  end_encoder();
  const size_t pitch = (image.width * 4 + 255) & ~size_t(255), bytes = pitch * image.height;
  if (!slot_->capture || slot_->capture.get().length < bytes) {
    id<MTLBuffer> buffer = [device_.get() newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    slot_->capture = Object<id<MTLBuffer>>(buffer); [buffer release];
    require(bool(slot_->capture), "Metal capture allocation failed");
  }
  slot_->readback_pending = true; slot_->capture_width = image.width;
  slot_->capture_height = image.height; slot_->capture_pitch = pitch;
  int rw = 0, rh = 0;
  slot_->capture_retina = mac_embed::retina_size(window_, &rw, &rh);
  id<MTLBlitCommandEncoder> encoder = [command_.get() blitCommandEncoder];
  [encoder copyFromTexture:image sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
    sourceSize:MTLSizeMake(image.width, image.height, 1) toBuffer:slot_->capture.get() destinationOffset:0
    destinationBytesPerRow:pitch destinationBytesPerImage:bytes];
  [encoder endEncoding];
}
#ifdef YOUGAME_AGENT_TEST_HOOK
void MetalBackend::agent_readback(id<MTLTexture> image, const Frame& frame) {
  auto& evidence = yougame::agent_present::recorder();
  if (!evidence.enabled()) return;
  AgentPresent pending;
  pending.present = presented_; pending.sim = frame.source_retrace;
  pending.major = frame.scene_major; pending.minor = frame.scene_minor;
  pending.width = image.width; pending.height = image.height;
  SDL_GetWindowSize(window_, &pending.window_width, &pending.window_height);
  if (evidence.wants_frame(presented_)) {
    PhaseTimer timing{phases_.readback};
    // Copy the actual finished BGRA drawable, including the letterbox, before
    // presenting it. Each drawable owns its copy even with several XFBs/frame.
    pending.pitch = (image.width * 4 + 255) & ~size_t(255);
    const size_t bytes = pending.pitch * image.height;
    id<MTLBuffer> buffer = [device_.get() newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    pending.pixels = Object<id<MTLBuffer>>(buffer); [buffer release];
    require(bool(pending.pixels), "Metal agent capture allocation failed");
    id<MTLBlitCommandEncoder> encoder = [command_.get() blitCommandEncoder];
    [encoder copyFromTexture:image sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
      sourceSize:MTLSizeMake(image.width, image.height, 1) toBuffer:pending.pixels.get() destinationOffset:0
      destinationBytesPerRow:pending.pitch destinationBytesPerImage:bytes];
    [encoder endEncoding];
  }
  agent_presents_.push_back(std::move(pending));
}
void MetalBackend::record_agent_presents() {
  auto& evidence = yougame::agent_present::recorder();
  const bool readback = std::any_of(agent_presents_.begin(), agent_presents_.end(),
      [](const AgentPresent& pending) { return bool(pending.pixels); });
  if (readback) {
    // Diagnostic pictures, like GL's glReadPixels, may wait for the GPU. With
    // present_frames off there is no extra GPU wait, copy or pixel processing.
    [slot_->fence.get() waitUntilCompleted];
    require(slot_->fence.get().status == MTLCommandBufferStatusCompleted, "Metal agent readback failed");
  }
  for (const auto& pending : agent_presents_) {
    yougame::agent_present::Image strip;
    if (pending.pixels) {
      const auto* bytes = static_cast<const uint8_t*>(pending.pixels.get().contents);
      std::vector<uint8_t> rgb(size_t(pending.width) * pending.height * 3);
      for (int y = 0; y < pending.height; ++y) {
        for (int x = 0; x < pending.width; ++x) {
          const auto* pixel = bytes + size_t(y) * pending.pitch + size_t(x) * 4;
          auto* out = rgb.data() + (size_t(y) * pending.width + x) * 3;
          out[0] = pixel[2]; out[1] = pixel[1]; out[2] = pixel[0];
        }
      }
      strip = yougame::agent_present::downscale(rgb.data(), pending.width, pending.height, 480, false);
    }
    // The command buffer (and each presentDrawable) is committed before this
    // hook. Use GL's source retrace, scene, dimensions, clocks and PPM writer.
    evidence.after_swap(pending.present, pending.sim, pending.major, pending.minor,
        pending.width, pending.height, pending.window_width, pending.window_height, std::move(strip));
  }
  agent_presents_.clear();
}
#endif
void MetalBackend::flush_capture(Slot& slot) {
  if (!slot.readback_pending) return;
  const auto* bytes = static_cast<const uint8_t*>(slot.capture.get().contents);
  std::vector<uint8_t> rgb(size_t(slot.capture_width) * slot.capture_height * 3);
  for (uint32_t y = 0; y < slot.capture_height; ++y) {
    for (uint32_t x = 0; x < slot.capture_width; ++x) {
      const auto* pixel = bytes + size_t(y) * slot.capture_pitch + size_t(x) * 4;
      auto* out = rgb.data() + (size_t(y) * slot.capture_width + x) * 3;
      out[0] = pixel[2]; out[1] = pixel[1]; out[2] = pixel[0];
    }
  }
  // Keep the activation from readback time: a fallback may occur before this slot retires.
  const auto output = mac_embed::limit_capture(rgb, slot.capture_width, slot.capture_height, slot.capture_retina);
  auto write = [&](const std::filesystem::path& path) {
    std::ofstream file(path, std::ios::binary);
    file << "P6\n" << output.w << ' ' << output.h << "\n255\n";
    file.write(reinterpret_cast<const char*>(rgb.data()), rgb.size()); file.close();
    if (!file) host::log("metal: capture write failed: %s", path.c_str());
    return bool(file);
  };
  if (!slot.capture_path.empty()) write(slot.capture_path);
  if (slot.test_shot) {
    if (test_all_) {
      // Preserve GL's :all hash of bottom-to-top RGB, without saving PPMs.
      const size_t pitch = size_t(output.w) * 3;
      for (int y = 0; y < output.h / 2; ++y)
        std::swap_ranges(rgb.begin() + y * pitch, rgb.begin() + (y+1) * pitch,
                         rgb.begin() + (output.h-1-y) * pitch);
      test_hashes_ << slot.test_present << ' ' << std::hex << hash_bytes(rgb.data(), rgb.size()) << std::dec << std::endl;
    } else if (write(test_dir_ / ("present-" + std::to_string(slot.test_present) + ".ppm"))) {
      test_shots_ << slot.test_present << ' ' << slot.test_sequence << ' '
        << output.w << 'x' << output.h << ' '
        << std::hex << hash_bytes(rgb.data(), rgb.size()) << std::dec << std::endl;
    }
  }
  slot.capture_path.clear(); slot.test_shot = false; slot.readback_pending = false;
}
void MetalBackend::update_widescreen() {
  int width = width_, height = height_;
  if (!mac_embed::drawable_size(window_, &width, &height)) SDL_GetWindowSize(window_, &width, &height);
  set_true_widescreen(options_.true_widescreen && !options_.widescreen,
                      true_widescreen_aspect(options_, width, height));
}
void MetalBackend::update_embed_scale() {
  int w = 0, h = 0;
  const bool retina = mac_embed::retina_size(window_, &w, &h);
  if (!embed_scale_.update(retina, w, h, [this](int& w, int& h) { SDL_GetWindowSize(window_, &w, &h); })) return;
  const int next = mac_embed::efb_scale(options_.efb_scale, h); // Metal's existing path has no SSAA multiplier.
  if (next == scale_) return;
  auto color = make_texture(EFB_WIDTH * next, EFB_HEIGHT * next, MTLPixelFormatRGBA8Unorm);
  auto depth = make_texture(EFB_WIDTH * next, EFB_HEIGHT * next, MTLPixelFormatDepth32Float);
  const BlitConstants full{{1, 1, 0, 0}, 0};
  const auto offset = slot_->ring.upload(&full, sizeof full);
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = color.get();
  pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.depthAttachment.texture = depth.get();
  pass.depthAttachment.loadAction = MTLLoadActionDontCare;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> encoder = [command_.get() renderCommandEncoderWithDescriptor:pass];
  [encoder setRenderPipelineState:retina_resize_pso_.get()];
  [encoder setDepthStencilState:clear_depth_.get()];
  [encoder setCullMode:MTLCullModeNone];
  [encoder setViewport:MTLViewport{0, 0, double(color.get().width), double(color.get().height), 0, 1}];
  [encoder setVertexBuffer:slot_->ring.buffer.get() offset:offset atIndex:0];
  [encoder setFragmentTexture:color_.get() atIndex:0];
  [encoder setFragmentTexture:depth_.get() atIndex:1];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [encoder endEncoding];
  color_ = std::move(color); depth_ = std::move(depth); scale_ = next;
  host::log("embed: Metal EFB x%d for %dx%d px", scale_, w, h);
}
void MetalBackend::submit_frame(const Frame& frame, const DrawMatrices* overrides) {
  @autoreleasepool {
    const auto start = Clock::now();
    phases_ = {};
    const auto fallback_before = fallback_draws_;
    const auto built_before = compiler_->built_.load();
    slot_ = &slots_[submitted_++ % 3];
    const auto wait = Clock::now();
    if (slot_->fence) {
      [slot_->fence.get() waitUntilCompleted];
      if (slot_->fence.get().status == MTLCommandBufferStatusError)
        throw std::runtime_error("Metal GPU command failed: " + error_text(slot_->fence.get().error));
    }
    const double fence_ms = milliseconds(wait);
    auto phase = Clock::now();
    flush_capture(*slot_); slot_->ring.used = 0;
    const double capture_ms = milliseconds(phase);
    phase = Clock::now();
    command_ = Object<id<MTLCommandBuffer>>([queue_.get() commandBuffer]);
    require(bool(command_), "Metal command buffer unavailable");
    command_.get().label = @"GX frame";
    update_embed_scale();
    update_widescreen();
    set_css_scene(frame.scene_major == 1 && frame.scene_minor == 0);
    set_main_projection(frame_scene_draw(frame));
    set_look_filter(LightweightFilter{});
    const double prepare_ms = milliseconds(phase);
    phase = Clock::now();
    const uint32_t present_before = presented_;
    try {
      const auto geometry = slot_->ring.upload(frame.vertices.data(), frame.vertices.size() * sizeof(Vertex));
      for (const auto& command : frame.commands) {
        if (command.kind == FrameCommand::Draw) {
          if (command.index >= frame.draws.size()) continue;
          draw(frame, frame.draws[command.index], overrides ? overrides + command.index : nullptr, geometry);
        } else {
          if (command.index >= frame.copies.size()) continue;
          const auto& efb = frame.copies[command.index];
          if (efb.to_xfb) { if (!skip_) present(frame, efb); }
          else copy(efb);
          if (efb.clear) clear(efb);
        }
      }
    } catch (...) {
      // Submit encoded uploads before reusing any ring storage, even on failure.
      end_encoder(); [command_.get() commit]; slot_->fence = command_; command_ = {};
      ++ring_failures_; throw;
    }
    end_encoder();
    // Nested presentation/readback phases are excluded from encode so these
    // phase durations can be added without double-counting nextDrawable.
    const double encode_ms = std::max(0., milliseconds(phase) - phases_.surface - phases_.drawable - phases_.readback);
    phase = Clock::now();
    [command_.get() commit]; slot_->fence = command_; command_ = {};
    const double commit_ms = milliseconds(phase);
    phase = Clock::now();
#ifdef YOUGAME_AGENT_TEST_HOOK
    record_agent_presents();
#endif
    const double evidence_ms = milliseconds(phase);
    phase = Clock::now();
    if (presented_ != present_before) {
      mac_embed::frame_presented(window_); host::window_presented(presented_);
    }
    const double notify_ms = milliseconds(phase);
    phase = Clock::now();
    compiler_->flush_compiler_log();
    const double drain_ms = milliseconds(phase);
    phase = Clock::now();
    if (submitted_ % 120 == 0) {
      for (auto it = textures_.begin(); it != textures_.end();) it = it->second.used + 300 < submitted_ ? textures_.erase(it) : std::next(it);
      for (auto it = copies_.begin(); it != copies_.end();) it = it->second.used + 300 < submitted_ ? copies_.erase(it) : std::next(it);
    }
    const double cleanup_ms = milliseconds(phase);
    // Preserve the existing prefix/fields for consumers; append disjoint CPU
    // phases. Total includes cleanup, but excludes this log and pool teardown.
    host::log("metal: submit simulation %llu scene %u:%u %.3f ms fence %.3f ms pipelines %u fallback %llu ring %zu"
              " surface %.3f ms drawable %.3f ms encode %.3f ms readback %.3f ms commit %.3f ms"
              " capture %.3f ms prepare %.3f ms evidence %.3f ms notify %.3f ms drain %.3f ms cleanup %.3f ms",
      (unsigned long long)frame.sequence, frame.scene_major, frame.scene_minor, milliseconds(start), fence_ms,
      compiler_->built_.load()-built_before, (unsigned long long)(fallback_draws_-fallback_before), slot_->ring.used,
      phases_.surface, phases_.drawable, encode_ms, phases_.readback, commit_ms,
      capture_ms, prepare_ms, evidence_ms, notify_ms, drain_ms, cleanup_ms);
  }
}
void MetalBackend::stop_compiler() {
  const auto deadline = Clock::now() + std::chrono::milliseconds(250);
  compiler_->stopping_.store(true);
  {
    std::lock_guard<std::mutex> lock(compiler_->mutex_);
    compiler_->jobs_.clear();
  }
  // Synchronize with the stage wait's predicate-to-sleep transition, so a
  // cancellation notify cannot be lost while a stage owner is in the driver.
  { std::lock_guard<std::mutex> lock(compiler_->stages_mutex_); }
  compiler_->wake_.notify_all(); compiler_->stages_wake_.notify_all();
  unsigned abandoned = 0;
  {
    std::unique_lock<std::mutex> lock(compiler_->mutex_);
    compiler_->finished_.wait_until(lock, deadline, [&] { return compiler_->active_ == 0; });
    abandoned = compiler_->active_;
  }
  // Even completed threads are detached: no unbounded join in shutdown or a
  // constructor unwind. Captured shared context outlives every worker access.
  for (auto& worker : workers_) if (worker.joinable()) worker.detach();
  workers_.clear();
  if (abandoned) host::log("metal: abandoned %u compiler workers after 250 ms shutdown budget", abandoned);
}
MetalBackend::~MetalBackend() {
  const auto deadline = Clock::now() + std::chrono::milliseconds(500);
  stop_compiler();
  @autoreleasepool {
    compiler_->flush_compiler_log();
    // Reused slots retire in submission order; drain the final three in that
    // order too, preserving :all's line ordering without per-frame GPU waits.
    Slot* ordered[] = {&slots_[0], &slots_[1], &slots_[2]};
    std::sort(std::begin(ordered), std::end(ordered), [](const Slot* a, const Slot* b) {
      return a->test_present < b->test_present;
    });
    for (auto* slot : ordered) {
      while (slot->fence && slot->fence.get().status < MTLCommandBufferStatusCompleted && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      if (!slot->fence || slot->fence.get().status == MTLCommandBufferStatusCompleted) flush_capture(*slot);
      else if (slot->readback_pending) host::log("metal: skipped pending capture at shutdown (GPU error or 500 ms budget)");
    }
    // Default retained-reference command buffers keep submitted resources alive
    // if the GPU has not finished. Never read unfinished diagnostic readbacks.
    host::log("metal: compiler: %u pipelines on worker, %u failed, 0 on frame thread; %u MSL cache hits, %u translation misses; fallback draws %llu; frame errors %llu",
      compiler_->built_.load(), compiler_->failed_.load(), compiler_->cache_hits_.load(), compiler_->translation_misses_.load(),
      (unsigned long long)fallback_draws_, (unsigned long long)ring_failures_);
    host::log("metal: functions: %u compiled, %u shared across UID aliases", compiler_->function_builds_.load(), compiler_->function_shares_.load());
  }
}
} // namespace
Backend* create_metal_backend(void* window, int width, int height, const RenderOptions& options) {
  try { return new MetalBackend(static_cast<SDL_Window*>(window), width, height, options); }
  catch (const std::exception& error) { throw std::runtime_error(std::string("Metal renderer: ") + error.what()); }
}
} // namespace gx
