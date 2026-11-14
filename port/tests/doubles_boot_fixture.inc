#include "yougame_console_engine.h"
#include "yougame_doubles.h"
#include "yougame_console_notice.h"
#include "native_savestate.h"
#include "mu_host_aram.h"
#include <cassert>
#include <cstdlib>
#include <set>
#include <map>
#include <functional>
#include <vector>
namespace host {
static uint8_t aram_bytes[MU_HOST_ARAM_BYTES];
uint8_t* aram=aram_bytes;
void log(const char*,...){}
void input_rumble(int,bool){}
void audio_reset_stream(){}
void window_pump(){}
struct Cpu { uint64_t tb=0; }; Cpu* cpu=nullptr;
std::function<void()> capture_native_boot_clock(){return []{};}
[[noreturn]] void die(const char*){std::abort();}
}
namespace screen_label { void show(const char*,double){} }
namespace gx { bool supports_game_reset(){return true;} std::function<void()> capture_boot_state(){return []{};} }
namespace slippi::online { static int mode=-1,cleanups=0; int session_mode(){return mode;} void native_cleanup_match(){++cleanups;} }
namespace source_p2p { bool requested(){return false;} }
namespace audio_core { struct State {int value=0;}; void reset(State&){} }
static audio_core::State g_audio;
static bool g_console_boot=false,g_replaying=false,g_resim=false,g_console_in_retrace=false;
static std::filesystem::path g_console_card;
static int g_online_test_mode=-1,g_console_host_pad=-1;
static unsigned g_handoff_disc_depth=0;
static unsigned callbacks(){return 0;}
static unsigned (*g_handoff_callbacks_pending)()=callbacks;
static std::vector<int> g_disc_loads;
static struct {void end(){}} g_savestates;
static int g_mem1_watch;
static bool mounted=false;
void origin_mount_card(const std::vector<uint8_t>& bytes){assert(bytes.empty());mounted=true;}
void origin_end_card_session(){mounted=false;}
void append_sound_driver_exclusions(std::vector<MuStateRegion>&){}
static unsigned game_bytes[16];
static struct {
  unsigned (*state_regions)(MuStateRegion*,unsigned);
  unsigned (*state_exclusions)(MuStateRegion*,unsigned);
  int (*practice)(int,int32_t*,int);
} g_game;
static std::shared_ptr<yougame::doubles::Launch> g_doubles_launch;
static std::atomic<uint64_t> g_console_catch_up{0};
static void console_host_catch_up(uint64_t) {}
// The production source supplies CssOriginRuntime before including doubles_boot.inc.
