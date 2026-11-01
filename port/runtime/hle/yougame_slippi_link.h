// Slippi linking presentation and asynchronous lifecycle; no window or account I/O.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <nlohmann/json.hpp>
#include "yougame_blend.h"
#include "slippi_account.h"
#include <atomic>
#include <chrono>
#include <future>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace yougame::slippi_linking {
using json = nlohmann::json;
inline constexpr int kTimeout = 600000;
inline constexpr const char* kSearching = "Searching YouGame players";
inline constexpr const char* kSearchCta = "Searching YouGame players - Y: Link Slippi";
inline constexpr const char* kRefusal = "Sign in to Slippi to play Direct, Teams and Party";
inline constexpr const char* kRefusalCta = "Direct, Teams and Party need Slippi - Y: Link Slippi";
inline constexpr const char* kOpen = "Link Slippi: finish in the YouGame window";
inline constexpr const char* kLinkedMenu = "Slippi linked - choose a mode to play";
inline constexpr const char* kLater = "Slippi linked - it starts next time you open the game";
enum class Screen { None, Search, Refusal };
enum class Answer { Now, Later, NoFile, Unlinked, Declined, Error, Timeout, Cancelled };
inline json request() { return {{"type", "slippiLink"}}; }
inline json response(const json& j) {
  if (!j.is_object() || !j.count("ok") || !j["ok"].is_boolean() || !j["ok"].get<bool>() ||
      !j.count("result") || !j["result"].is_object()) return j;
  auto result = j["result"];
  if (!result.count("ok")) result["ok"] = true;
  return result;
}
struct Result {
  Answer kind;
  std::filesystem::path file;
  Result(Answer value, std::filesystem::path path = {}) : kind(value), file(std::move(path)) {}
};
inline Result parse_answer(const json& raw) {
  const auto j = response(raw);
  if (!j.is_object()) return Answer::Error;
  for (const char* key : {"ok", "linked", "now"})
    if (!j.count(key) || !j[key].is_boolean()) return Answer::Error;
  if (!j["ok"].get<bool>()) return Answer::Error;
  const auto declined = j.find("declined");
  if (declined != j.end() && declined->is_boolean() && declined->get<bool>()) return Answer::Declined;
  if (!j["linked"].get<bool>()) return Answer::Unlinked;
  if (!j["now"].get<bool>()) return Answer::Later;
  const auto member = j.find("file");
  if (member == j.end() || !member->is_string()) return Answer::NoFile;
  const auto text = member->get<std::string>();
  if (text.empty() || text.find('\0') != std::string::npos) return Answer::NoFile;
  try {
    auto file = std::filesystem::u8path(text);
    auto name = file.filename().u8string();
#ifdef _WIN32
    // Only the ASCII filename is case-insensitive; preserve the app's full path verbatim.
    for (char& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
#endif
    if (file.is_absolute() && name == "user.json") return {Answer::Now, std::move(file)};
  } catch (const std::filesystem::filesystem_error&) {}
  return Answer::NoFile;
}
inline Answer answer(const json& raw) { return parse_answer(raw).kind; }
inline const char* log_line(Answer a) {
  switch (a) {
    case Answer::Now: return "slippi link: linked now";
    case Answer::Later: return "slippi link: linked for next start";
    case Answer::NoFile: return "slippi link: linked, no file";
    case Answer::Unlinked: return "slippi link: not linked";
    case Answer::Declined: return "slippi link: declined for this run";
    case Answer::Timeout: return "slippi link: not linked (timeout)";
    case Answer::Cancelled: return "slippi link: not linked (cancelled)";
    default: return "slippi link: not linked (error)";
  }
}
struct Presentation {
  std::atomic<bool> enabled{false}, open{false}, later{false}, now{false}, declined{false};
  std::string line(Screen screen) const {
    if (screen == Screen::None) return {};
    if (!enabled || declined) return screen == Screen::Search ? kSearching : kRefusal;
    if (later) return kLater;
    if (now) return screen == Screen::Search ? blend::kSearchBoth : kLinkedMenu;
    if (open) return kOpen;
    return screen == Screen::Search ? kSearchCta : kRefusalCta;
  }
};
inline Presentation presentation;
// Only simulation-thread code owns Flow. Presentation is also read by the renderer/MM workers.
// Keep a cancelled worker until it retires, so a second screen cannot send a concurrent request.
class Flow {
 public:
  explicit Flow(Presentation& p) : p_(p) {}
  ~Flow() { if (stop_) *stop_ = true; }
  bool pending() const { return future_.valid(); }
  template<class Send>
  bool press(Screen screen, uint32_t scene, Send send) {
    if (!p_.enabled || p_.declined || p_.later || p_.now || screen == Screen::None || future_.valid()) return false;
    stop_ = std::make_shared<std::atomic<bool>>(false);
    scene_ = scene;
    cancelled_ = false;
    p_.open = true;
    std::packaged_task<Result()> task([send, stop = stop_]() -> Result {
      try {
        const auto j = send(request(), kTimeout, [stop] { return stop->load(); });
        return stop->load() ? Result(Answer::Cancelled) : parse_answer(j);
      } catch (const std::exception& e) {
        if (stop->load()) return Answer::Cancelled;
        // Only fixed reasons leave this path; app errors can contain account details.
        return std::string(e.what()) == "The YouGame app did not answer" ? Answer::Timeout : Answer::Error;
      } catch (...) { return Answer::Error; }
    });
    future_ = task.get_future();
    std::thread(std::move(task)).detach();
    return true;
  }
  template<class Log>
  void cancel(Log log) {
    if (!future_.valid() || cancelled_) return;
    cancelled_ = true;
    *stop_ = true;
    p_.open = false;
    log(log_line(Answer::Cancelled));
  }
  template<class Login, class Restart, class Log>
  void tick(uint32_t scene, bool match, Login login, Restart restart, Log log) {
    if (!future_.valid()) return;
    if (scene != scene_ || match) cancel(log);
    if (future_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    const auto reply = future_.get();
    const auto result = reply.kind;
    p_.open = false;
    if (cancelled_) return;
    if (result == Answer::Declined) p_.declined = true;
    if (result == Answer::Later || result == Answer::NoFile) p_.later = true;
    if (result == Answer::Now) p_.now = true;
    log(log_line(result));
    if (result == Answer::Now) {
      // The worker only carries the app's answer. Install after cancellation/scene checks,
      // before login and the new search, and never expose the path to presentation or logs.
      slippi::account::set_app_sign_in(reply.file);
      login(); restart();
    }
  }
 private:
  Presentation& p_;
  uint32_t scene_ = 0;
  bool cancelled_ = false;
  std::shared_ptr<std::atomic<bool>> stop_;
  std::future<Result> future_;
};
} // namespace yougame::slippi_linking

namespace yougame {
// Same bridge as Friends; called only by the background linking worker.
nlohmann::json request_slippi_link(const nlohmann::json& request, int timeout, const std::function<bool()>& cancelled);
}
