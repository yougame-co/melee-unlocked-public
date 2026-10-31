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
#ifdef __APPLE__
  CHECK(std::string(app::settings_backend_in_use("D3D12")) == "OpenGL");
  CHECK(std::string(app::settings_backend_in_use("D3D11")) == "OpenGL");
#else
  CHECK(std::string(app::settings_backend_in_use("D3D12")) == "D3D12");
  CHECK(std::string(app::settings_backend_in_use("D3D11")) == "D3D11");
#endif
  std::puts("7 checks passed");
}
