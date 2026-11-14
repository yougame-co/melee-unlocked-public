"""Graphics-only launch stays ahead of game services and preserves normal-launch behavior."""
from pathlib import Path
import unittest
ROOT = Path(__file__).resolve().parents[2]
def source(name):
    return (ROOT / name).read_text(encoding="utf-8-sig")

def body(text, marker):
    start = text.index("{", text.index(marker))
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start + 1:end - 1]

class PrewarmOnlyWiring(unittest.TestCase):
    def setUp(self):
        self.text = source("port/app/main.cpp")
        self.main = self.text[self.text.index("static int melee_main(int argc, char** argv) {"):]
        self.prewarm = body(self.text, "static int run_prewarm_only(")

    def test_entry_precedes_services(self):
        route = self.main.index("if (app::prewarm_only::requested(argc, argv)) {")
        for service in ("yougame::start();", "source_port::reserve_memory()", "source_port::run(shutdown)", "host::disc_open(o.iso)", "host::audio_open("):
            self.assertLess(route, self.main.index(service))
        for required in ("gx::load_pc_settings(gfx, volume);", "prepare_render_backend(gfx);",
                         'L"Melee Unlocked prewarm", false', "gfx.pc_settings = false;",
                         "gfx.fullscreen = gfx.exclusive_fullscreen = false;",
                         'flush_all_result("prewarm-only", std::chrono::seconds(30))',
                         'host::log("prewarm-only: backend=%s recipes=%llu cache_hits=%llu compiled=%llu ms=%lld saved=%s"',
                         "gx::cache_store::flush_result_name(saved)", "app::prewarm_only::with_cache_lock(cache"):
            self.assertIn(required, self.prewarm)
        self.assertLess(self.prewarm.index("with_cache_lock(cache"), self.prewarm.index("host::window_create("))
        for forbidden in ("audio_open", "disc_open", "source_port::run", "yougame::start", "create_threaded_backend"):
            self.assertNotIn(forbidden, self.prewarm)

    def test_hidden_and_complete_startup(self):
        sdl = source("port/linux/window_sdl.inl")
        self.assertIn('SDL_SetHintWithPriority(SDL_HINT_MAC_BACKGROUND_APP, on ? "1" : "0", SDL_HINT_OVERRIDE)', sdl)
        self.assertIn("visible ? SDL_WINDOW_SHOWN : SDL_WINDOW_HIDDEN", sdl)
        self.assertIn("host::window_prefer_background(true);", self.prewarm)
        self.assertIn("void window_prefer_background(bool) {}", source("port/runtime/host/window.cpp"))
        gl = source("port/linux/renderer_gl.cpp")
        self.assertIn("if (prewarm_only) sync_shaders_ = true;", gl)
        self.assertIn("prewarm_only ? std::numeric_limits<double>::max()", gl)
        self.assertIn("if (prewarm_only) recipes_written_ = false;", gl)
        self.assertLess(gl.index("bool recipes_written_ = true;"), gl.index("// Last member"))
        for path in ("port/runtime/gx/gx_d3d12.cpp", "port/runtime/gx/gx_vulkan.cpp", "port/linux/renderer_gl.cpp"):
            self.assertIn("prewarm_stats() const override", source(path))

    def test_graphics_settings_precede_early_return(self):
        settings = body(source("port/runtime/gx/pc_settings.cpp"), "void load_pc_settings(")
        early = settings.index("if (options.prewarm_only) return;")
        for graphics in ("options.true_aspect = TrueAspect::FitWindow;", "options.true_widescreen = true;",
                         "migrate_lightweight(options, lowspec_look)", "set_lightweight_level(options.lightweight)",
                         "dlss5::profile_set_folder(options.settings_path)", "texpack::set_disabled_packs"):
            self.assertLess(settings.index(graphics), early)
        for service in ("// The player's Gecko codes live", "source_port::mods::set_choices", "host::phob_store_load"):
            self.assertLess(early, settings.index(service))

    def test_normal_vulkan_path_is_static_and_cwd_relative(self):
        vulkan = source("port/runtime/gx/gx_vulkan.cpp")
        # Build 66 (upstream v0.9.104): the Vulkan cache is <shader cache>/vulkan-v1, with the folder set by
        # vulkan_set_shader_cache (the default stays the cwd-relative shadercache). The prewarm locks that folder.
        self.assertEqual(body(vulkan, "std::filesystem::path shader_cache_dir()"), '''
  std::lock_guard<std::mutex> lock(g_cache_mutex);   // the render thread and the pipeline worker both ask
  static std::filesystem::path made;
  const std::filesystem::path p = g_cache_root / "vulkan-v1";
  if (p != made) { std::error_code ec; std::filesystem::create_directories(p, ec); made = p; }
  return p;
''')
        self.assertIn('g_cache_root = folder.empty() ? std::filesystem::path("shadercache")', vulkan)
        for text in (vulkan, source("port/runtime/gx/gx_vulkan.h"), self.text):
            self.assertNotIn("vulkan_shader_cache_root", text)
        self.assertIn('const auto cache = gfx.api == gx::RenderApi::Vulkan ? root / "vulkan-v1" : root;', self.prewarm)
        self.assertIn("gx::vulkan_set_shader_cache(gfx.shader_cache);", body(self.text, "static void prepare_render_backend("))
        self.assertIn("same working directory as a normal launch", body(self.text, "static void usage()"))
        self.assertIn("// --prewarm-only must run from the same working directory as a normal launch.", self.main)

    def test_prewarm_does_not_touch_vulkan_recovery(self):
        prepare = body(self.text, "static void prepare_render_backend(")
        self.assertIn("if (!gfx.prewarm_only && gfx.api == gx::RenderApi::Vulkan && gx::vulkan_failed_starts()", prepare)
        vulkan = source("port/runtime/gx/gx_vulkan.cpp")
        ctor = body(vulkan, "VulkanBackend(const VulkanWindow&")
        self.assertIn('if (!opts_.prewarm_only) { std::error_code ec; std::filesystem::remove(shader_cache_dir() / "startup.pending", ec); }', ctor)
        create = body(vulkan, "void create_device()")
        guarded = body(create, "if (!opts_.prewarm_only)")
        self.assertIn("const int failed = vulkan_failed_starts();", guarded)
        self.assertIn("std::ofstream(marker", guarded)
        self.assertIn('entry.path().filename() != "prewarm.lock"', guarded)
        self.assertNotIn("vulkan_failed_starts", create.replace(guarded, ""))
        lost = body(vulkan, "if (submitted == VK_ERROR_DEVICE_LOST)")
        self.assertIn('std::ofstream(shader_cache_dir() / "startup.pending"', body(lost, "if (!opts_.prewarm_only)"))

    def test_crash_and_exit_handling(self):
        self.assertIn("g_crash_dialog = false;", self.prewarm)
        self.assertIn("app::prewarm_only::install_crash_handlers();", self.prewarm)
        self.assertIn("SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);", self.prewarm)
        self.assertIn("host::set_die_hook(app::prewarm_only::die_hook<host::end_process>", self.prewarm)
        entry = body(self.main, "if (app::prewarm_only::requested(argc, argv))")
        self.assertIn("host::log_flush();", entry)
        self.assertIn("host::end_process(result);", entry)
        self.assertNotIn("#ifdef _WIN32", entry)
        self.assertIn("if (status != app::prewarm_only::save_timeout) host::window_destroy();", self.prewarm)
        self.assertIn("if (!g_crash_dialog) return;", source("port/linux/full/crash_linux.inl"))
        self.assertIn("if (g_crash_dialog)", self.text)

    def test_logs_usage_and_unsupported(self):
        self.assertIn("host::options.log_file = app::prewarm_only::log_path(args.log_file);", self.prewarm)
        self.assertLess(self.prewarm.index("log_path({})"), self.prewarm.index("with_arguments("))
        self.assertIn("return app::prewarm_only::unsupported_backend;", self.prewarm)
        win = source("port/runtime/gx/gx_backend.cpp")
        self.assertIn("throw UnsupportedPrewarmBackend", win)
        self.assertIn("if (options.prewarm_only) return nullptr;", win)
        usage = body(self.text, "static void usage()")
        for status in ("0 ok", "2 no renderer", "3 write failed", "4 already running", "5 save timeout", "6 unsupported backend", "64 usage"):
            self.assertIn(status, usage)
            self.assertIn(status, source("port/app/prewarm_only.h"))

if __name__ == "__main__":
    unittest.main()
