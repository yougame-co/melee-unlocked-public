// Offline address policy; one primary and optional public fallback per remote seat.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
namespace slippi {
struct RemoteAddress {
  std::string first, fallback;
};
inline RemoteAddress remote_address(const std::string& local_external_ip,
                                    const std::string& external, const std::string& lan) {
  if (external.substr(0,external.find(':')) == local_external_ip && !lan.empty() && lan != external)
    return {lan,external};
  return {external,{}};
}
// Numeric IPv4 "a.b.c.d:port" only (wire data is never resolved through DNS). The address is
// stored in network byte order and the port in host order, as ENetAddress keeps them.
template <class Address>
inline bool public_endpoint(const std::string& endpoint, Address& out) {
  const auto colon = endpoint.find(':');
  if (colon == std::string::npos) return false;
  uint8_t octets[4] = {}; int part = 0; unsigned value = 0; int digits = 0;
  for (size_t n = 0; n <= colon; ++n) {
    const char c = n < colon ? endpoint[n] : '.';
    if (c >= '0' && c <= '9') {
      if (++digits > 3 || (value = value * 10 + unsigned(c - '0')) > 255) return false;
    } else if (c == '.' && digits && part < 4) {
      octets[part++] = uint8_t(value); value = 0; digits = 0;
    } else return false;
  }
  if (part != 4) return false;
  unsigned port = 0; digits = 0;
  for (size_t n = colon + 1; n < endpoint.size(); ++n) {
    if (endpoint[n] < '0' || endpoint[n] > '9' || ++digits > 5) return false;
    port = port * 10 + unsigned(endpoint[n] - '0');
  }
  if (!digits || port == 0 || port > 65535) return false;
  std::memcpy(&out.host, octets, sizeof octets); out.port = uint16_t(port);
  return true;
}
inline bool public_fallback_due(uint64_t elapsed_ms, bool connected, bool started, bool available) {
  return available && !connected && !started && elapsed_ms >= 2000;
}
} // namespace slippi
