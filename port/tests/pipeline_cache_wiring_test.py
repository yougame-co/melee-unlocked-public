"""Shader and pipeline caches are saved while the game runs and before every exit or relaunch.

The caches used to be written only from the renderers' destructors. A Friends join relaunches the
game and the desktop app ends a game that has not exited 5 s after it asked, so the save often never
happened and the next start compiled every pipeline again. These checks keep the wiring in place:
saves happen on the cache saver's thread (pipeline_cache_store.h), the exit and relaunch paths
flush first, every backend logs one line per save and per session, and the first-start compile
shows a progress frame instead of a frozen window. A large copy never holds the locks pipeline
builds take, timed saves wait out a match, and crash paths freeze every save first.
"""
from pathlib import Path
import re
import unittest

from yougame_console_wiring_test import body

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig").replace("\r\n", "\n")


def between(text, start, end):
    at = text.index(start)
    return text[at:text.index(end, at)]


def class_tail(text, member):
    """The class must end right after the saver: members are destroyed last-first, so the saver
    (and its final save) goes before anything its producers read."""
    at = text.index(member) + len(member)
    return text[at:at + 8].strip().startswith("};")


D3D12 = "port/runtime/gx/gx_d3d12.cpp"
VULKAN = "port/runtime/gx/gx_vulkan.cpp"
OPENGL = "port/linux/renderer_gl.cpp"


class PipelineCacheWiring(unittest.TestCase):
    def test_store_writes_atomically_off_the_caller(self):
        store = source("port/runtime/gx/pipeline_cache_store.h")
        write = body(store, "write_file_atomic")
        self.assertLess(write.index('".tmp"'), write.index("std::filesystem::rename(temporary, path, ec)"))
        self.assertIn("std::filesystem::remove(temporary, ec)", write)
        self.assertIn("thread_ = std::thread([this] { run(); });", store)
        # Producers run without the saver's own lock: a producer may take a backend lock that a
        # thread marking the saver dirty already holds.
        save = body(store, "save")
        locks = [m.start() for m in re.finditer("lock_guard", save)]
        self.assertEqual(len(locks), 2)   # taking the queue, then putting failed writes back
        first_scope_end = save.index("}", save.index("queued.swap(queue_);"))
        self.assertLess(first_scope_end, save.index("file.producer(file.out)"))
        self.assertLess(save.index("file.producer(file.out)"), locks[1])
        # A failed write is tried again; an exception from a producer never leaves the thread.
        self.assertIn("file.failed()", save)
        self.assertIn("catch (...)", save)
        self.assertIn("std::this_thread::get_id() == thread_.get_id()", store)
        # The buffer a producer fills is kept between saves (no new zero-filled 100 MB each time).
        self.assertIn("Buffer out;", store)
        self.assertIn("new uint8_t[capacity]", body(store, "resize"))
        # Frozen: checked before the queue, before every producer, and right before the rename.
        self.assertTrue(save.lstrip().startswith("if (frozen()) return false;"))
        self.assertLess(save.index("if (frozen()) return false;", save.index("for (auto& file : files_)")),
                        save.index("file.producer(file.out)"))
        self.assertLess(write.index("ok = ok && !frozen();"), write.index("std::filesystem::rename(temporary, path, ec)"))
        self.assertIn("std::atomic<bool>::is_always_lock_free", store)
        # A rename Windows refuses for a moment (antivirus, the relaunched game) is tried again.
        self.assertIn("detail::rename_busy(ec)", write)
        self.assertIn("ec.value() == 32", body(store, "rename_busy"))
        # A flush reports a failed write instead of counting it done.
        self.assertIn("if (ok) flush_ok_ = target;", body(store, "run"))
        self.assertIn("wait_flushed_result(target, deadline) == FlushResult::Ok", body(store, "wait_flushed"))
        self.assertIn("return flush_ok_ >= target ? FlushResult::Ok : FlushResult::Failed;", body(store, "wait_flushed_result"))
        self.assertIn("return FlushResult::Timeout;", body(store, "wait_flushed_result"))
        # Stale temporaries are listed on the saver thread, never in start() (the render thread).
        self.assertNotIn("remove_stale_temporaries", body(store, "start"))
        self.assertIn("remove_stale_temporaries_in(folder)", body(store, "run"))
        # A hold (a match) keeps timed saves back; flushes and the last save do not wait for it.
        run = body(store, "run")
        self.assertIn("if (held_) { cv_.wait(lk); continue; }", run)
        self.assertLess(run.index("if (stopping_)"), run.index("if (held_)"))
        self.assertLess(run.index("if (flush_requested_ > flush_done_)"), run.index("if (held_)"))

    def test_d3d12_saves_incrementally_and_never_from_a_frame(self):
        d3d = source(D3D12)
        self.assertNotIn("save_pipeline_library", d3d)
        self.assertNotIn("void D3D12Backend::save_pipeline_recipes", d3d)
        destructor = between(d3d, "~D3D12Backend() override {", "bool supports_game_reset()")
        self.assertLess(destructor.index("stop_pso_workers();"), destructor.index("cache_saver_.stop();"))
        self.assertIn('log_pipeline_stats("session"', destructor)
        start = body(d3d, "D3D12Backend::start_cache_saver")
        self.assertIn('"/recipes.bin"', start)
        self.assertIn('"/pipelines.bin"', start)
        self.assertIn("cache_saver_.start();", start)
        constructor = between(d3d, "D3D12Backend(HWND hwnd", "~D3D12Backend()")
        self.assertLess(constructor.index("start_cache_saver();"), constructor.index("prewarm_pipelines();"))
        self.assertLess(constructor.index("prewarm_pipelines();"), constructor.index("recipes_ready_ = true;"))
        build = body(d3d, "D3D12Backend::build_pso")
        self.assertIn("++pipelines_loaded_;", build)
        self.assertIn("++pipelines_compiled_;", build)
        self.assertIn("pipeline_library_dirty_ = true; cache_saver_.mark_dirty();", build)
        library = body(d3d, "D3D12Backend::produce_pipeline_library")
        # Only the state checks and the size are taken under the lock pipeline builds take: the copy
        # (and its buffer) come after the locked scope.
        locked = library[library.index("std::lock_guard<std::mutex> lk(pipeline_library_mutex_);"):]
        locked = locked[:locked.index("pipeline_library_dirty_ = false;")]
        self.assertIn("guarded_serialized_size", locked)
        self.assertNotIn("guarded_serialize_library", locked)
        self.assertNotIn("data.resize", locked)
        copy = library.index("guarded_serialize_library(library.Get()")
        self.assertLess(library.index("pipeline_library_dirty_ = false;"), library.index("data.resize((size_t)size);"))
        self.assertLess(library.index("data.resize((size_t)size);"), copy)
        self.assertLess(library.index("if (pipeline_library_faulted_)"), copy)
        self.assertIn("pipeline_library_fault(call);", library)   # a fault in the copy is marked under the lock
        self.assertIn("return cache_store::Produce::Failed;", library)   # outgrew the buffer: tried again
        self.assertIn("return cache_store::Produce::Remove;", library)
        self.assertNotIn("fopen", library)   # the saver writes the bytes after the lock is released
        # Recipes: only what is new is copied under the lock the render thread takes.
        recipes = body(d3d, "D3D12Backend::produce_pipeline_recipes")
        self.assertIn("fresh.assign(pipeline_recipes_.begin() + recipes_copied_", recipes)
        self.assertNotIn("= pipeline_recipes_;", recipes)
        self.assertIn("recipes_set_hash_ == recipes_file_set_hash_", recipes)   # an unchanged list is not written again
        # Timed saves wait out a match; a lost device freezes the savers before anything else.
        self.assertIn("cache_saver_.hold(in_match_);", body(d3d, "D3D12Backend::submit_frame"))
        lost = d3d[d3d.index("if ((code == 0x887A0005u"):]
        self.assertLess(lost.index("::gx::cache_store::freeze();"), lost.index("GetDeviceRemovedReason"))
        # The settings overlay never reaches ImGui's unchecked upload on a lost device.
        overlay = d3d[d3d.index("if (const HRESULT removed = device_->GetDeviceRemovedReason()"):]
        self.assertLess(overlay.index("check(removed,"), overlay.index("settings_ui_->draw(list_.Get());"))
        # A device lost mid-upload: ImGui's texture upload skips instead of mapping a null buffer (builds 82/83).
        imgui = (ROOT / "port/third_party/imgui/backends/imgui_impl_dx12.cpp").read_text(encoding="utf-8")
        upload = imgui[imgui.index("IID_PPV_ARGS(&bd->pTexUploadBuffer)"):]
        self.assertLess(upload.index("if (FAILED(hr) || bd->pTexUploadBuffer == nullptr)"),
                        upload.index("bd->pTexUploadBuffer->Map(0, &range, &bd->pTexUploadBufferMapped)"))
        self.assertIn("if (FAILED(hr) || bd->pTexUploadBufferMapped == nullptr)", upload)
        self.assertNotIn("IM_ASSERT(SUCCEEDED(hr));\n\n            D3D12_RANGE range", imgui)
        self.assertIn("__try { *hr = library->Serialize(data, size); return true; }", d3d)
        self.assertEqual(d3d.count("pipeline_recipes_.push_back"), 1)   # only inside remember_recipe
        self.assertIn("pipeline_recipes_.push_back", body(d3d, "D3D12Backend::remember_recipe"))
        self.assertIn("cache_saver_.mark_dirty();", body(d3d, "D3D12Backend::pipeline_library_fault"))
        self.assertTrue(class_tail(d3d, "cache_store::Saver cache_saver_;"))

    def test_vulkan_saves_incrementally_and_counts_cache_hits(self):
        vk = source(VULKAN)
        self.assertNotIn("void save_pipeline_cache", vk)
        self.assertNotIn("void save_pipeline_recipes", vk)
        destructor = between(vk, "~VulkanBackend() override {", "if (host::process_ending()) return;")
        self.assertLess(destructor.index("stop_pipeline_worker();"), destructor.index("cache_saver_.stop();"))
        self.assertIn('log_pipeline_stats("session"', destructor)
        start = body(vk, "start_cache_saver")
        self.assertIn('"vk_recipes.bin"', start)
        self.assertIn('"pipelines.bin"', start)
        make = body(vk, "make_game_pipeline")
        self.assertIn("VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT", make)
        self.assertIn("cache_saver_.mark_dirty();", make)
        cache = body(vk, "produce_pipeline_cache")
        self.assertIn("vkGetPipelineCacheData", cache)
        # A copy that fails gives the dirty mark back (Failed runs the failed-write path), and a cache
        # that grew between the size and the copy (VK_INCOMPLETE) is asked again.
        self.assertIn("if (copied != VK_INCOMPLETE) break;", cache)
        self.assertIn("return cache_store::Produce::Failed;", cache)
        self.assertIn("[this] { cache_dirty_ = true; }", start)
        self.assertIn("cache_saver_.hold(frame_in_match(frame));", vk)
        self.assertIn("if (r == VK_ERROR_DEVICE_LOST) cache_store::freeze();", vk)
        lost = vk[vk.index("if (submitted == VK_ERROR_DEVICE_LOST) {"):]
        self.assertTrue(lost[lost.index("{") + 1:].lstrip().startswith("cache_store::freeze();"))
        progress = body(vk, "show_progress")
        self.assertLess(progress.index("host::window_pump();"), progress.index("if (!swapchain_"))
        self.assertIn("if (!recipes_ready_)", body(vk, "produce_recipes"))
        self.assertNotIn("recipes_.push_back", vk.replace(body(vk, "remember_recipe"), ""))
        prewarm = body(vk, "prewarm_pipelines")
        self.assertLess(prewarm.index("prewarm_recipes();"), prewarm.index("recipes_ready_ = true;"))
        self.assertIn("show_progress(done++, total);", body(vk, "prewarm_recipes"))
        self.assertTrue(class_tail(vk, "cache_store::Saver cache_saver_;"))

    def test_opengl_writes_programs_on_the_saver_thread(self):
        gl = source(OPENGL)
        save_binary = body(gl, "save_binary")
        self.assertIn("cache_saver_.enqueue(", save_binary)
        self.assertNotIn("write_cache(", save_binary)
        destructor = between(gl, "~Renderer() override {", "if (!context_) return;")
        self.assertIn("cache_saver_.stop();", destructor)
        self.assertNotIn("void save_recipes", gl)
        self.assertIn("show_progress(i, count);", gl)
        self.assertIn("cache_saver_.add_folder(", body(gl, "start_cache_saver"))
        # A hidden or minimized window is skipped, and a progress swap never waits for vsync.
        progress = body(gl, "show_progress")
        self.assertLess(progress.index("SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED"), progress.index("SDL_GL_SwapWindow"))
        self.assertLess(progress.index("SDL_GL_SetSwapInterval(0)"), progress.index("SDL_GL_SwapWindow"))
        self.assertLess(progress.index("SDL_GL_SwapWindow"), progress.index("SDL_GL_SetSwapInterval(interval)"))
        self.assertTrue(class_tail(gl, "gx::cache_store::Saver cache_saver_;"))

    def test_every_backend_logs_one_line_per_save_and_session(self):
        formats = {
            D3D12: '"d3d12: pipelines cache_hits=%llu compiled=%llu saved_bytes=%llu (%s)"',
            VULKAN: '"vulkan: pipelines cache_hits=%llu compiled=%llu unreported=%llu saved_bytes=%llu (%s)"',
            OPENGL: '"opengl: pipelines cache_hits=%llu compiled=%llu saved_bytes=%llu (%s)%s"',
        }
        for path, fmt in formats.items():
            text = source(path)
            with self.subTest(path=path):
                self.assertIn(fmt, text)
                self.assertIn("cache_saver_.on_saved(", text)
                self.assertIn('log_pipeline_stats("session"', text)
                self.assertIn('log_pipeline_stats("startup"', text)

    def test_exit_and_relaunch_flush_before_the_process_can_end(self):
        main = source("port/app/main.cpp")
        shutdown = main[main.index("auto shutdown = [](int) {"):]
        # After the process-ending mark (a driver thread throwing during the flush ends quietly),
        # before the renderer is torn down. The source-port exit asks first and waits last, so the
        # Friends Leave goes out while the caches are written, inside the app's 5 s.
        request = shutdown.index('::gx::cache_store::request_flush_all("exit")')
        wait = shutdown.index("::gx::cache_store::wait_flush_all(caches, caches_due);")
        self.assertLess(shutdown.index("host::set_process_ending();"), request)
        self.assertLess(request, shutdown.index('console_engine().end("window closed")'))
        self.assertLess(shutdown.index("yougame::console::wait_friends_retired(2000);"), wait)
        self.assertLess(wait, shutdown.index("->reset();"))
        self.assertNotIn("flush_all(", shutdown[:shutdown.index("->reset();")].replace("request_flush_all(", "").replace("wait_flush_all(", ""))
        exit_path = main[main.index("} catch (const ExitRequested& stop) {"):]
        flush = exit_path.index('::gx::cache_store::flush_all("exit"')
        self.assertLess(exit_path.index("host::set_process_ending();"), flush)
        self.assertLess(flush, exit_path.index("backend.reset();"))
        # Crash paths freeze the savers first: no save starts or renames a file from a crashing process.
        for name in ("crash_filter", "report_unhandled"):
            with self.subTest(handler=name):
                self.assertTrue(body(main, name).lstrip().startswith("::gx::cache_store::freeze();"))
        crash_linux = source("port/linux/full/crash_linux.inl")
        self.assertTrue(body(crash_linux, "linux_crash_signal").lstrip().startswith("::gx::cache_store::freeze();"))
        host = source("port/runtime/host/host.cpp")
        end = body(host, "end_process")
        self.assertLess(end.index("set_process_ending();"), end.index('::gx::cache_store::flush_all("exit"'))
        self.assertLess(end.index('::gx::cache_store::flush_all("exit"'), end.index("TerminateProcess"))
        self.assertIn("if (!g_dying.load())", end)   # never from a crash
        self.assertTrue(body(host, "die").lstrip().startswith("::gx::cache_store::freeze();\n  g_dying.store(true);"))
        restart = body(host, "request_restart")
        self.assertLess(restart.index('::gx::cache_store::flush_all("relaunch"'), restart.index("CreateProcessW("))
        looks = body(host, "restart_with_mod_looks")
        self.assertLess(looks.index('::gx::cache_store::flush_all("relaunch"'), looks.index("CreateProcessW("))
        link = source("port/runtime/hle/yougame_link.cpp")
        entry = body(link, "friends_entry")
        self.assertLess(entry.index('::gx::cache_store::flush_all("relaunch"'), entry.index("prepared = handoff(c,relaunching"))
        # Asked for as the room opens, so the flush at the handoff usually has nothing left to write.
        opened = entry[entry.index("io.opened = [](const std::string& code) {"):]
        opened = opened[:opened.index("};")]
        self.assertIn('if (g_stdio && g_relaunch) ::gx::cache_store::request_flush_all("relaunch");', opened)
        for text in (main, host, link):
            self.assertIn('#include "pipeline_cache_store.h"', text)

    def test_first_start_shows_progress_not_a_frozen_window(self):
        frame = source("port/runtime/gx/progress_frame.h")
        self.assertIn('"Preparing graphics for this PC (first start only)"', frame)
        self.assertIn('L"Preparing graphics for this PC (first start only)"', source(D3D12))
        vk = body(source(VULKAN), "show_progress")
        self.assertIn("progress_frame::layout(", vk)
        self.assertIn("vkCmdClearAttachments", vk)
        self.assertIn("vkQueuePresentKHR", vk)
        gl = body(source(OPENGL), "show_progress")
        self.assertIn("progress_frame::layout(", gl)
        self.assertIn("SDL_GL_SwapWindow", gl)

    def test_unit_test_is_registered(self):
        for path in ("CMakeLists.txt", "port/CMakeLists.txt"):
            text = source(path)
            with self.subTest(path=path):
                self.assertIn("pipeline_cache_store_test", text)
                self.assertTrue(re.search(r"add_test\(NAME port_pipeline_cache_store\b", text))


if __name__ == "__main__":
    unittest.main()
