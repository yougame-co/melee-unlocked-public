#include "settings_log.h"
#include <cstdio>
#include <string>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
  const std::string tail = ", frame cap monitor, sub-frame off, internal resolution auto, upscaler 0, vsync on, windowed or borderless";
  CHECK("settings: " + app::settings_backend_label("D3D12", "D3D12") + tail ==
        "settings: D3D12" + tail);
  CHECK("settings: " + app::settings_backend_label("D3D11", "D3D11") + tail ==
        "settings: D3D11" + tail);
  CHECK("settings: " + app::settings_backend_label("D3D12", "OpenGL") + tail ==
        "settings: saved D3D12, using OpenGL" + tail);
  CHECK(app::settings_backend_label("D3D11", "OpenGL") == "saved D3D11, using OpenGL");
  CHECK(app::settings_backend_label("OpenGL", "OpenGL") == "OpenGL");
  CHECK(app::settings_backend_label("Vulkan", "D3D12") == "saved Vulkan, using D3D12");
  CHECK(app::settings_backend_label("D3D12", "none (headless)") == "saved D3D12, using none (headless)");
  std::puts("7 checks passed");
}
