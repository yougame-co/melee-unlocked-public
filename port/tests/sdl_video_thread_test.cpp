#include "sdl_video_thread.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::abort(); } } while (0)

int main() {
  using Call = host::SdlVideoThread::Call;
  host::SdlVideoThread video;
  std::atomic<int> logs{0}, work{0}, bad_message{0};
  auto log = [&](const char* message) {
    if (std::strcmp(message, "window: pump skipped on a non-video thread")) ++bad_message;
    ++logs;
  };
  // Uninitialized video must not let a headless/bootstrap caller touch SDL.
  CHECK(!video.allow(Call::Pump, log));
  CHECK(logs == 1);
  video.claim();
  CHECK(video.allow(Call::Pump, log));
  const std::array<Call, 7> kinds{Call::Pump, Call::EmbedCommands, Call::Event,
      Call::LoadingShow, Call::LoadingClose, Call::Destroy, Call::Title};
  std::vector<std::thread> callers;
  for (int i = 0; i < 16; ++i) callers.emplace_back([&] {
    for (int repeat = 0; repeat < 100; ++repeat)
      for (auto kind : kinds) if (video.allow(kind, log)) ++work;
    video.release(); // a non-owner cannot discard the owner's identity
  });
  for (auto& caller : callers) caller.join();
  CHECK(work == 0);
  CHECK(logs == 7);
  CHECK(bad_message == 0);
  for (auto kind : kinds) CHECK(video.allow(kind, log));
  video.release();
  CHECK(!video.allow(Call::Pump, log));
  // Recreation has a new owner but retains the process-wide per-kind log suppression.
  std::thread replacement([&] {
    video.claim();
    if (video.allow(Call::Pump, log)) ++work;
    video.release();
  });
  replacement.join();
  CHECK(work == 1);
  CHECK(!video.allow(Call::Pump, log));
  CHECK(logs == 7);
  std::printf("sdl_video_thread: %d checks; 16 callers x 100 passes x 7 kinds\n", checks);
}
