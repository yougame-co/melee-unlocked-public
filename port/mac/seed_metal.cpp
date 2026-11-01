// Offline MSL seed builder. No Metal device, window, game, or Apple toolchain.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gx_metal_translate.h"
#include "gx_metal_recipe.h"
#include "../runtime/gx/gx_shader.h"
#include <iostream>
#include <map>

using Recipe = gx::metal::GLRecipe;
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
struct Stage { bool vertex; std::string source; std::vector<std::string> names; };
int main(int argc, char** argv) {
  try {
    if (argc != 4) throw std::runtime_error("usage: seed-metal <GL recipes> <output directory> <workers 1..4>");
    const int workers = std::stoi(argv[3]);
    if (workers < 1 || workers > 4) throw std::runtime_error("workers must be 1..4");
    const auto data = shader_translation::read_text(argv[1]);
    uint64_t header[3]{};
    if (data.size() < sizeof header) throw std::runtime_error("missing recipes");
    std::memcpy(header, data.data(), sizeof header);
    if (header[0] != 0x3150524C47505847ull || !header[1] || header[1] > 16384 ||
        data.size() != sizeof header + header[1] * sizeof(Recipe) ||
        gx::hash_bytes(data.data() + sizeof header, data.size() - sizeof header) != header[2])
      throw std::runtime_error("incompatible recipes/ABI/checksum");
    const std::filesystem::path output = argv[2];
    std::filesystem::create_directories(output);
    const auto started = Clock::now();
    std::map<std::string, bool> names;
    std::map<std::string, size_t> sources;
    std::vector<Stage> stages;
    uint64_t rejected = 0;
    std::string valid_data(sizeof header, '\0');
    double generation_ms = 0;
    for (uint64_t i = 0; i < header[1]; ++i) {
      Recipe recipe;
      std::memcpy(&recipe, data.data() + sizeof header + i * sizeof recipe, sizeof recipe);
      if (!gx::metal::valid_recipe(gx::metal::gl_recipe_inputs(recipe))) { ++rejected; continue; }
      valid_data.append(reinterpret_cast<const char*>(&recipe), sizeof recipe);
      gx::DrawCall draw{}; draw.components = recipe.components; draw.bp = recipe.bp;
      std::memcpy(draw.xf_regs, recipe.xf, sizeof recipe.xf);
      const auto request = gx::metal::make_recipe(draw);
      for (bool vertex : {true, false}) {
        const uint64_t uid = vertex ? request.vs.hash() : request.ps.hash();
        char hex[17]; std::snprintf(hex, sizeof hex, "%016llx", (unsigned long long)uid);
        const auto name = std::string(vertex ? "vs-" : "ps-") + hex;
        if (!names.emplace(name, vertex).second) continue;
        const auto phase = Clock::now();
        const auto source = vertex ? gx::generate_vertex_shader(request.vs) : gx::generate_pixel_shader(request.ps);
        generation_ms += ms(phase);
        // Different GX UIDs can generate identical source. Translate it once,
        // then materialize every runtime UID filename; no hash-collision alias.
        const auto key = std::string(vertex ? "v" : "p") + source;
        auto [found, inserted] = sources.emplace(key, stages.size());
        if (inserted) stages.push_back({vertex, source, {name}});
        else stages[found->second].names.push_back(name);
      }
    }
    if (rejected) std::cerr << "metal: skipped " << rejected << " invalid recipe records in " << argv[1] << '\n';
    header[1] -= rejected;
    header[2] = gx::hash_bytes(valid_data.data() + sizeof header, valid_data.size() - sizeof header);
    std::memcpy(valid_data.data(), header, sizeof header);
    std::atomic<size_t> next{0}, misses{0};
    std::atomic<bool> failed{false};
    std::mutex log_mutex;
    std::vector<std::thread> pool;
    for (int worker = 0; worker < workers; ++worker) pool.emplace_back([&] {
      for (;;) {
        const size_t i = next.fetch_add(1);
        if (i >= stages.size() || failed) break;
        try {
          const auto& stage = stages[i];
          const auto phase = Clock::now();
          const bool hit = !shader_translation::read_text(output / (stage.names.front() + ".metal")).empty();
          if (!hit) ++misses;
          const auto text = gx::metal_translation::translate(output, stage.names.front(), stage.vertex, stage.source);
          for (const auto& name : stage.names) shader_translation::write_text(output / (name + ".metal"), text);
          std::lock_guard<std::mutex> lock(log_mutex);
          std::cout << "stage " << stage.names.front() << " aliases " << stage.names.size()
                    << " cache " << hit << " ms " << ms(phase) << '\n';
        } catch (const std::exception& error) {
          failed = true;
          std::lock_guard<std::mutex> lock(log_mutex); std::cerr << error.what() << '\n';
        }
      }
    });
    for (auto& worker : pool) worker.join();
    if (failed) return 1;
    shader_translation::write_text(output / "recipes.bin", valid_data);
    uint64_t bytes = 0;
    for (const auto& entry : names) bytes += std::filesystem::file_size(output / (entry.first + ".metal"));
    const auto manifest = std::string("{\n  \"cache_version\": \"msl-3\",\n  \"recipe_records\": ") + std::to_string(header[1]) +
      ",\n  \"runtime_stage_keys\": " + std::to_string(names.size()) +
      ",\n  \"distinct_sources\": " + std::to_string(stages.size()) +
      ",\n  \"msl_bytes\": " + std::to_string(bytes) + "\n}\n";
    shader_translation::write_text(output / "manifest.json", manifest);
    std::cout << "RESULT {\"workers\":" << workers << ",\"records\":" << header[1]
              << ",\"stage_keys\":" << names.size() << ",\"sources\":" << stages.size()
              << ",\"translation_misses\":" << misses << ",\"generation_ms\":" << generation_ms
              << ",\"wall_ms\":" << ms(started) << ",\"msl_bytes\":" << bytes << "}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
