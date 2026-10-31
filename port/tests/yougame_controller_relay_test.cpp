// The app's relayed pads in the game's controller families (yougame_controller_relay.h), read
// through host::controller_relay the way window.cpp reads them. No window, devices or game.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "yougame_controller_relay.h"
#include <cstdio>
#include <fstream>
#include <string>
using namespace host;
namespace {
int failures = 0, checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (0)
using Json = nlohmann::json;
Json relay_pad(unsigned slot, const char* kind, unsigned family_index, uint32_t b, unsigned lt = 0, unsigned rt = 0) {
  return {{"slot", slot}, {"familyIndex", family_index}, {"id", "p" + std::to_string(slot + 1)}, {"name", "Pad"},
          {"kind", kind}, {"b", b}, {"lx", 32767}, {"ly", -32768}, {"rx", 0}, {"ry", 0}, {"lt", lt}, {"rt", rt}};
}
std::string pads_line(const Json& pads) { return "yg-pads " + Json({{"seq", 1}, {"t", 1}, {"pads", pads}}).dump(); }
controller_relay::Snapshot bridged(const Json& pads) {
  yougame::relay_pads::Frame frame; std::string reason;
  CHECK(yougame::relay_pads::parse(pads_line(pads), frame, reason));
  return yougame::relay_pads::controller_snapshot(frame);
}
const controller_relay::Sample* device_of(const controller_relay::Snapshot& s, CaptureDevice family, int index) {
  for (const auto& d : s.devices) if (d.family == family && d.index == index) return &d;
  return nullptr;
}
// As yougame_link.cpp installs it.
yougame::relay_pads::State g_relay_state([](const std::string&) {});
controller_relay::Snapshot relayed_snapshot() { return yougame::relay_pads::controller_snapshot(g_relay_state.snapshot()); }

std::ifstream fixture(const char* name) {
  // Copied verbatim from controller-lead's controller/{gap-run-lines.txt,fixtures/ps.jsonl}.
  std::ifstream stream(std::filesystem::path(__FILE__).parent_path() / "fixtures" / "relay" / name);
  CHECK(stream.is_open());
  return stream;
}
InputDeviceFacts relay_facts() {
  InputDeviceFacts facts;
  const auto snapshot = relayed_snapshot();
  for (const auto& device : snapshot.devices) {
    if (device.index < 0 || device.index >= 4) continue;
    switch (device.family) {
      case CaptureDevice::XInputPad: facts.xinput[device.index] = true; break;
      case CaptureDevice::DS4Pad: facts.ds4[device.index] = true; break;
      case CaptureDevice::SwitchPro: facts.switch_pro[device.index] = true; break;
      case CaptureDevice::HidPad: facts.hid[device.index] = true; break;
      default: break;
    }
  }
  return controller_relay::fallback_facts(facts);
}
void gap_run() {
  auto stream = fixture("gap-run-lines.txt");
  controller_relay::install(relayed_snapshot);
  std::string line;
  unsigned count = 0;
  while (std::getline(stream, line)) {
    if (line.rfind("yg-pads ", 0) != 0) continue;
    ++count;
    CHECK(g_relay_state.accept(line, count * 1000));
    const auto frame = g_relay_state.snapshot();
    const auto snapshot = relayed_snapshot();
    CHECK(frame.seq == count);
    for (unsigned seat = 0; seat < 4; ++seat) {
      const bool present = seat == 0 || seat == 2 || (seat == 1 && count == 1);
      CHECK(frame.pads[seat].present == present);
      const auto& device = snapshot.devices[seat];
      if (!present) { CHECK(device.family == CaptureDevice::None); continue; }
      CHECK(frame.pads[seat].id == "p" + std::to_string(seat + 1));
      const auto family = seat == 0 ? CaptureDevice::DS4Pad : CaptureDevice::XInputPad;
      const int index = seat == 2 ? 1 : 0;
      CHECK(device.family == family && device.index == index);
      controller_relay::Sample sampled;
      CHECK(controller_relay::sample(family, index, sampled));
      const uint32_t expected = count == 3 ? (seat == 0 ? uint32_t(DS4_CROSS) : 0x1000u) : 0u;
      CHECK(sampled.buttons == expected);
    }
    controller_relay::Sample missing;
    CHECK(!controller_relay::sample(CaptureDevice::XInputPad, 2, missing));
    CHECK(!controller_relay::sample(CaptureDevice::DS4Pad, 1, missing));
    CHECK(controller_relay::sample(CaptureDevice::XInputPad, 0, missing) == (count == 1));
  }
  CHECK(count == 4);
  controller_relay::install(nullptr);
}
void virtual_fallback() {
  controller_relay::install(relayed_snapshot);
  auto keyboard = relay_pad(0, "other", 0, 0x1000);
  keyboard["id"] = "virtual";
  auto sources = default_port_sources();
  const auto route = [&](const Json& pads, const std::array<PortSource, 4>& configured, int port = 0) {
    CHECK(g_relay_state.accept(pads_line(pads), 1));
    return effective_port_source(configured, relay_facts(), port);
  };
  for (const char* kind : {"ps", "switch", "xbox", "other", "hid"}) {
    const auto real = relay_pad(2, kind, 1, 0);
    const auto real_kind = kind == std::string("ps") ? DeviceKind::DS4Pad :
      kind == std::string("switch") ? DeviceKind::SwitchPro :
      kind == std::string("hid") ? DeviceKind::HidPad : DeviceKind::XInputPad;
    auto source = route(Json::array({keyboard, real}), sources);
    CHECK(source.kind == real_kind);
    CHECK(source.index == 1);
    // It is still connected and can be explicitly selected on P1 or another port.
    controller_relay::Sample sample;
    CHECK(controller_relay::sample(CaptureDevice::XInputPad, 0, sample) && sample.buttons == 0x1000);
    auto explicit_sources = sources;
    explicit_sources[0] = {DeviceKind::XInputPad, 0};
    source = route(Json::array({keyboard, real}), explicit_sources);
    CHECK(source.kind == DeviceKind::XInputPad && source.index == 0);
    explicit_sources[1] = {DeviceKind::XInputPad, 0};
    source = route(Json::array({keyboard, real}), explicit_sources, 1);
    CHECK(source.kind == DeviceKind::XInputPad && source.index == 0);
    // A real pad already assigned elsewhere still excludes virtual from automatic P1.
    explicit_sources = sources;
    explicit_sources[1] = {real_kind, 1};
    source = route(Json::array({keyboard, real}), explicit_sources);
    CHECK(source.kind == DeviceKind::Keyboard);
  }
  auto source = route(Json::array({keyboard}), sources);
  CHECK(source.kind == DeviceKind::XInputPad && source.index == 0);
  auto ps = relay_pad(2, "ps", 0, 0);
  ps["id"] = "virtual"; // The exception identifies kind and id together.
  source = route(Json::array({keyboard, ps}), sources);
  CHECK(source.kind == DeviceKind::DS4Pad && source.index == 0);
  source = route(Json::array(), sources);
  CHECK(source.kind == DeviceKind::Keyboard);
  // Only the exact other/virtual identity is excluded; another generic pad keeps Xbox priority.
  keyboard["id"] = "keyboard";
  source = route(Json::array({keyboard, relay_pad(2, "ps", 0, 0)}), sources);
  CHECK(source.kind == DeviceKind::XInputPad && source.index == 0);
  controller_relay::install(nullptr);
}
void ps_fixture() {
  auto stream = fixture("ps.jsonl");
  controller_relay::install(relayed_snapshot);
  unsigned count = 0;
  std::string line;
  while (std::getline(stream, line)) {
    const auto entry = Json::parse(line);
    const auto& device = entry.at("device");
    // The device describes native masks; the line contains the parser's XInput-position wire form.
    CHECK(device.at("kind") == "ps" && device.at("buttons") == Json::array({"DS4_CROSS"}));
    CHECK(g_relay_state.accept(entry.at("line").get<std::string>(), 1000));
    controller_relay::Sample sample;
    CHECK(controller_relay::sample(CaptureDevice::DS4Pad, device.at("index").get<int>(), sample));
    CHECK(sample.buttons == DS4_CROSS);
    unsigned axis = 0;
    for (const char* name : {"lx", "ly", "rx", "ry"}) CHECK(sample.axes[axis++] == device.at("axes").at(name).get<int>());
    CHECK(sample.left == device.at("triggers").at("l2").get<int>());
    CHECK(sample.right == device.at("triggers").at("r2").get<int>());
    CHECK(entry.at("profile").is_null());
    PadBindings defaults{}; defaults.mask[(size_t)BindAction::A] = DS4_CROSS; // window.cpp ds4_default_mask
    auto pad = controller_relay::raw_pad(sample);
    ds4_apply_bindings(defaults, static_cast<uint16_t>(sample.buttons), pad);
    CHECK(pad.button == 0x100);
    const auto source = effective_port_source(default_port_sources(), relay_facts(), entry.at("port").get<int>() - 1);
    CHECK(source.kind == DeviceKind::DS4Pad && source.index == device.at("index").get<int>());
    ++count;
  }
  CHECK(count == 1);
  controller_relay::install(nullptr);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2) {
    const std::string name = argv[1];
    if (name == "gap") gap_run();
    else if (name == "virtual") virtual_fallback();
    else if (name == "ps-fixture") ps_fixture();
    else CHECK(false);
    std::printf("yougame controller relay %s: %d checks, %d failures\n", argv[1], checks, failures);
    return failures ? 1 : 0;
  }
  // Seats 0 and 2 only: each family keeps its own numbers, and a gap stays a gap.
  auto s = bridged(Json::array({relay_pad(0, "ps", 0, 0x1000, 79, 20), relay_pad(2, "xbox", 0, 0x1000, 0, 255)}));
  const auto* ps = device_of(s, CaptureDevice::DS4Pad, 0);
  const auto* xb = device_of(s, CaptureDevice::XInputPad, 0);
  CHECK(ps && xb);
  CHECK(!device_of(s, CaptureDevice::XInputPad, 2) && !device_of(s, CaptureDevice::DS4Pad, 2));
  CHECK(s.devices[1].family == CaptureDevice::None && s.devices[3].family == CaptureDevice::None);
  // PlayStation: its own mask (south = Cross), L2 past the press point, no travel below it.
  if (ps) { CHECK(ps->buttons == (DS4_CROSS | DS4_L2)); CHECK(ps->left == 79 && ps->right == 0); }
  // Xbox: what the app sent, triggers raw, as before.
  if (xb) CHECK(xb->buttons == 0x1000 && xb->left == 0 && xb->right == 255);
  // A custom PlayStation Cross -> B table gives B, never the Xbox default A.
  if (ps) {
    PadBindings custom{}; custom.mask[(int)BindAction::B] = DS4_CROSS;
    PadState p = controller_relay::raw_pad(*ps);
    ds4_apply_bindings(custom, (uint16_t)ps->buttons, p);
    CHECK((p.button & 0x300) == 0x200);
  }
  // Switch: by position (south = B, east = A, west = Y, north = X); ZL/ZR are buttons with no travel.
  s = bridged(Json::array({relay_pad(1, "switch", 3, 0x1000 | 0x2000 | 0x4000 | 0x8000, 255, 255)}));
  const auto* sw = device_of(s, CaptureDevice::SwitchPro, 3);
  CHECK(sw);
  if (sw) {
    CHECK(sw->buttons == (SWPRO_B | SWPRO_A | SWPRO_Y | SWPRO_X | SWPRO_ZL | SWPRO_ZR));
    CHECK(sw->left == 0 && sw->right == 0);
  }
  // Raw HID: its own 32 bits (bit 31 = the hat's Left), its own family.
  s = bridged(Json::array({relay_pad(0, "hid", 0, 0x80000001u)}));
  const auto* hid = device_of(s, CaptureDevice::HidPad, 0);
  CHECK(hid && hid->buttons == 0x80000001u);
  // One number twice in the Xbox family (other + xbox): the lower seat plays, the other is left out.
  s = bridged(Json::array({relay_pad(3, "xbox", 1, 0x2000), relay_pad(0, "other", 1, 0x1000)}));
  const auto* one = device_of(s, CaptureDevice::XInputPad, 1);
  CHECK(one && one->buttons == 0x1000);
  CHECK(s.devices[3].family == CaptureDevice::None);

  // Through the provider: installed with nothing accepted yet, it owns its families with no pads.
  controller_relay::install(relayed_snapshot);
  controller_relay::Sample sample;
  CHECK(controller_relay::active() && !controller_relay::sample(CaptureDevice::XInputPad, 0, sample));
  CHECK(g_relay_state.accept(pads_line(Json::array({relay_pad(0, "ps", 0, 0x1000), relay_pad(2, "xbox", 0, 0)})), 1));
  CHECK(controller_relay::sample(CaptureDevice::DS4Pad, 0, sample) && sample.buttons == DS4_CROSS);
  CHECK(controller_relay::sample(CaptureDevice::XInputPad, 0, sample) && sample.buttons == 0);
  CHECK(!controller_relay::sample(CaptureDevice::XInputPad, 2, sample));
  PadState pads[4]{}; uint32_t buttons[4]{};
  CHECK(controller_relay::poll(CaptureDevice::DS4Pad, pads, buttons) == 1u && buttons[0] == DS4_CROSS);
  CHECK(controller_relay::poll(CaptureDevice::SwitchPro, pads, buttons) == 0u);
  CHECK(!g_relay_state.owns_hid());   // window.cpp leaves HID to the game's own reader
  // EOF: the relay keeps its latch and the provider stays in, with every family empty.
  g_relay_state.reset_on_eof();
  for (auto family : {CaptureDevice::XInputPad, CaptureDevice::DS4Pad, CaptureDevice::SwitchPro, CaptureDevice::HidPad})
    for (int i = 0; i < 4; ++i) CHECK(!controller_relay::sample(family, i, sample));
  CHECK(controller_relay::active() && g_relay_state.active());
  controller_relay::install(nullptr);
  std::printf("yougame controller relay: %d failures\n", failures);
  return failures ? 1 : 0;
}
