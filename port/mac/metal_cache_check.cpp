// GPU-free checks for Metal temporary-file ownership, pruning and unwind.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gx_metal_cache.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

static unsigned checks = 0;
static void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
  ++checks;
}
static void touch(const std::filesystem::path& path) {
  std::ofstream file(path); file << "fixture";
  if (!file) throw std::runtime_error("cannot write fixture");
}
int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("usage: metal-cache-check <new fixture directory>");
    const std::filesystem::path root = argv[1];
    if (!std::filesystem::create_directory(root)) throw std::runtime_error("fixture directory must be new");
    const std::vector<std::string> owned{
      "pipelines-0.123.000000000000abcd.pending.metalar", "pipelines-3.123.000000000000abcd.pending.metalar",
      "recipes.123.000000000000abcd.pending.bin", "vs-0123456789abcdef.123.456.hlsl",
      "vs-0123456789abcdef.123.456.spv", "vs-0123456789abcdef.123.456.metal",
      "vs-0123456789abcdef.123.456.log", "ps-0123456789abcdef.123.0.metal"};
    const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::minutes(11);
    for (const auto& name : owned) {
      check(gx::metal::owned_temporary_name(name), "owned name not recognized");
      touch(root / name); std::filesystem::last_write_time(root / name, old);
    }
    const std::vector<std::string> keep{
      "recipes.bin", "pipelines-0.metalar", "vs-0123456789abcdef.metal", "ps-0123456789abcdef.metal",
      "recipes.123.abcd.pending.bin", "pipelines-4.123.000000000000abcd.pending.metalar",
      "vs-0123456789abcdeZ.123.456.metal", "vs-0123456789abcdef.0.456.metal",
      "vs-0123456789abcdef.123.abcd.metal", "vs-0123456789abcdef.123.456.metal.extra",
      "foreign.pending.bin", "recipes.123.000000000000abcd.pending.bin.extra"};
    for (const auto& name : keep) {
      check(!gx::metal::owned_temporary_name(name), "foreign/final name matches temp pattern");
      touch(root / name); std::filesystem::last_write_time(root / name, old);
    }
    const auto fresh = root / "ps-0123456789abcdef.123.456.metal";
    touch(fresh);
    const auto directory = root / "vs-0123456789abcdef.123.789.metal";
    std::filesystem::create_directory(directory);
    std::filesystem::last_write_time(directory, old);
    const auto nested = directory / owned.front();
    touch(nested); std::filesystem::last_write_time(nested, old);
    const auto link = root / "vs-0123456789abcdef.123.987.metal";
    std::filesystem::create_symlink("recipes.bin", link);
    check(gx::metal::prune_temporary_files(root) == owned.size(), "wrong stale temp count");
    for (const auto& name : owned) check(!std::filesystem::exists(root / name), "stale temp survived");
    for (const auto& name : keep) check(std::filesystem::is_regular_file(root / name), "foreign/final file removed");
    check(std::filesystem::exists(fresh), "fresh temp removed");
    check(std::filesystem::exists(nested), "pruning recursed into subdirectory");
    check(std::filesystem::is_symlink(link), "symlink removed/followed");
    const auto pending = root / owned[5], input = root / owned[3], spv = root / owned[4], log = root / owned[6];
    try {
      const gx::metal::TemporaryFiles temporary{pending, input, spv, log};
      for (const auto& path : {pending, input, spv, log}) touch(path);
      throw std::runtime_error("simulated translation failure/cancellation");
    } catch (const std::runtime_error&) {}
    for (const auto& path : {pending, input, spv, log}) check(!std::filesystem::exists(path), "unwind left a temp file");
    {
      const gx::metal::TemporaryFiles temporary{pending};
      touch(pending);
      std::error_code ec;
      std::filesystem::rename(pending, directory, ec); // file cannot replace a directory
      check(bool(ec), "fixture rename unexpectedly succeeded");
    }
    check(!std::filesystem::exists(pending), "failed rename left temp");
    {
      const gx::metal::TemporaryFiles temporary{pending};
      touch(pending);
      std::filesystem::rename(pending, root / "vs-0123456789abcdef.metal");
    }
    check(std::filesystem::is_regular_file(root / "vs-0123456789abcdef.metal"), "cleanup removed completed MSL");
    check(gx::metal::prune_temporary_files(root / "missing") == 0, "missing directory not handled");
    std::cout << "Metal cache checks: " << checks << '/' << checks << " passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
