// HLSL -> SPIR-V -> MSL, called exclusively by Metal compiler workers and the offline seed builder.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "../linux/shader_translation.h"
#include "platform_paths.h"
#include "gx_metal_cache.h"
#include <cstdint>

namespace gx::metal_translation {
// Metal owns this helper so its warning flag is safe across four compiler workers.
// Keep GL's translation helper and output untouched.
inline void run_tool_concurrent(std::vector<std::string> args, const std::filesystem::path& log, const std::filesystem::path& tools,
                                const std::atomic<bool>* stopping = nullptr) {
  // Prefer shipped tools. M2's copied Homebrew glslang has a relative dylib
  // rpath that breaks after copying; only that dyld failure falls back locally.
  std::vector<std::string> candidates;
  std::error_code ec;
  const auto shipped = tools / args[0];
  if (std::filesystem::is_regular_file(shipped, ec)) candidates.push_back(shipped.string());
  const std::string homebrew = "/opt/homebrew/bin/" + args[0];
  if (std::filesystem::is_regular_file(homebrew, ec)) candidates.push_back(homebrew);
  candidates.push_back(args[0]); // PATH is the final fallback.
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  for (size_t index = 0; index < candidates.size(); ++index) {
    if (stopping && stopping->load()) throw std::runtime_error("Metal translation stopping");
    argv[0] = candidates[index].data();
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_addopen(&files, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_adddup2(&files, STDOUT_FILENO, STDERR_FILENO);
    pid_t child = 0;
    const int error = posix_spawnp(&child, argv[0], &files, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&files);
    if (error) {
      if ((error == ENOENT || error == ENOEXEC) && index + 1 < candidates.size()) continue;
      throw std::runtime_error("Cannot run " + candidates[index] + ": " + std::strerror(error));
    }
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
      const pid_t waited = waitpid(child, &status, WNOHANG);
      if (waited == child) break;
      if (waited < 0 && errno != EINTR) throw std::runtime_error("waitpid failed for shader compiler");
      if ((stopping && stopping->load()) || std::chrono::steady_clock::now() >= deadline) {
        kill(child, SIGKILL); // Only the helper PID this call spawned.
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        if (stopping && stopping->load()) throw std::runtime_error("Metal translation cancelled; killed owned helper PID");
        throw std::runtime_error("Shader helper timed out after 15s; killed owned PID " + std::to_string(child));
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (WIFEXITED(status) && !WEXITSTATUS(status)) return;
    const auto failure = shader_translation::read_text(log);
    if (index + 1 < candidates.size() && failure.find("dyld[") != std::string::npos &&
        failure.find("Library not loaded:") != std::string::npos) {
      static std::atomic<bool> warned{false};
      if (!warned.exchange(true)) {
        std::fprintf(stderr, "Mac shaders: copied tool lacks dylibs; using local Homebrew/PATH fallback\n");
      }
      continue;
    }
    throw std::runtime_error(candidates[index] + " failed:\n" + failure);
  }
}

inline std::string translate(const std::filesystem::path& cache, const std::string& name,
                             bool vertex, const std::string& source, bool force = false,
                             const std::atomic<bool>* stopping = nullptr) {
  using namespace shader_translation;
  const auto output = cache / (name + ".metal");
  if (!force) { if (auto text = read_text(output); !text.empty()) return text; }
  auto check_running = [&] {
    if (stopping && stopping->load()) throw std::runtime_error("Metal translation stopping");
  };
  check_running();
  // Distinct contexts may overlap briefly after bounded renderer shutdown.
  const auto prefix = cache / (name + "." + std::to_string(getpid()) + "." +
                               std::to_string(reinterpret_cast<uintptr_t>(stopping)));
  const auto input = prefix.string() + ".hlsl";
  const auto spv = prefix.string() + ".spv";
  const auto pending = prefix.string() + ".metal";
  const auto log = prefix.string() + ".log";
  const metal::TemporaryFiles temporary{input, spv, pending, log};
  std::string hlsl = source;
  if (!vertex) {
    // GX emits only constant texture indices and these two Sample coordinate
    // forms (regular TEV and indirect). Keep its b1 layout untouched; b2 is
    // Metal-only float4[2], packed like the eight float ring upload.
    hlsl = "cbuffer MetalSamplerBlock : register(b2) { float4 metal_lod_bias[2]; };\n" + hlsl;
    for (unsigned i = 0; i < 8; ++i) {
      const auto index = std::to_string(i);
      replace_all(hlsl, "Tex[" + index + "].Sample(samp[" + index + "],",
                       "Tex[" + index + "].SampleBias(samp[" + index + "],");
      replace_all(hlsl, "* texdims[" + index + "].xy)",
                       "* texdims[" + index + "].xy, metal_lod_bias[" +
                       std::to_string(i / 4) + "][" + std::to_string(i % 4) + "])");
    }
  }
  write_text(input, hlsl);
  const auto tools = mac_host::resources() / "bin";
  run_tool_concurrent({"glslangValidator", "-D", "-V", "--auto-map-locations", "--auto-map-bindings",
            "--hlsl-iomap", "-e", "main", "-S", vertex ? "vert" : "frag", "-o", spv, input}, log, tools, stopping);
  check_running();
  // Metal and D3D12 both use top-left fragment coordinates and 0..1 clip depth.
  // Preserve b0/b1, t0..7 and s0..7 in Metal's separate binding namespaces;
  // geometry occupies vertex buffer 30, outside the generated constant buffers.
  run_tool_concurrent({"spirv-cross", spv, "--msl", "--msl-version", "20100", "--msl-decoration-binding",
            "--rename-entry-point", "main", "gx_main", vertex ? "vert" : "frag",
            "--output", pending}, log, tools, stopping);
  check_running();
  auto text = read_text(pending);
  if (text.empty()) throw std::runtime_error("SPIRV-Cross produced empty MSL");
  std::filesystem::rename(pending, output);
  return text;
}
} // namespace gx::metal_translation
