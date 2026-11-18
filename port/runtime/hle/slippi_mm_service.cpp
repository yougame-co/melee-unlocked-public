// Slippi 3.7.0 matchmaking service client (see slippi_mm_service.h). Ported from Slippi's Rust
// matchmaking crate: client.rs (the session), protocol.rs (the messages) and stun.rs.
//
// The WebSocket is RFC 6455 over an IPv4-only connection, so the address the service observes is
// the IPv4 one netplay uses. Windows: Winsock and Schannel (TLS with the system's certificate
// checks). Linux and macOS: libcurl's connect-only mode (TCP and TLS, CURL_IPRESOLVE_V4), the
// transport port/linux/compat/winhttp_ws.cpp already uses there.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_mm_service.h"
#include "test_env.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <security.h>
#include <schannel.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "secur32.lib")
#else
#include <curl/curl.h>
#include <poll.h>
#endif
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <utility>

// host/host.h. Declared here so the unit test links this file without the host.
namespace host { void log(const char* fmt, ...); }

namespace slippi::mm_service {

using json = nlohmann::json;

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

const char* const kProdUrl = "wss://matchmaking.slippi.gg/v1/queue";
const char* const kDevUrl = "wss://matchmaking-dev.slippi.gg/v1/queue";
const char* const kUrlEnvVar = "SLIPPI_MM_URL";

// client.rs
constexpr milliseconds kConnectTimeout{5000};
constexpr milliseconds kHandshakeReadTimeout{10000};
constexpr milliseconds kPollReadTimeout{250};
constexpr int kConnectAttempts = 3;
// How long after losing a queued ticket's connection resuming is tried. The service drops a
// silent ticket after thirty seconds.
constexpr milliseconds kResumeWindow{15000};
constexpr milliseconds kRetryDelay{1000};
// How long a connection may go without the service acknowledging its join or resume.
constexpr milliseconds kJoinTimeout{5000};
// The service sends a status message every few seconds while a ticket is queued. After this long
// without any frame a ping goes out, and after kLivenessTimeout the connection counts as lost.
constexpr milliseconds kPingAfter{5000};
constexpr milliseconds kLivenessTimeout{15000};
// Not in client.rs (its socket writes have no limit): a stalled send gives up after this long.
constexpr milliseconds kWriteTimeout{10000};
// tungstenite's frame limit; a bigger frame or message is a protocol error.
constexpr size_t kMaxMessage = size_t(16) << 20;

const char* const kFailedConnect = "Failed to connect to mm server";
const char* const kLostConnection = "Lost connection to the mm server";
const char* const kFailedJoin = "Failed to join mm queue";
const char* const kServerError = "Received an error from the mm server";

// stun.rs
constexpr uint32_t kMagicCookie = 0x2112A442u;
constexpr uint16_t kBindingRequest = 0x0001, kBindingSuccess = 0x0101;
constexpr uint16_t kAttrMappedAddress = 0x0001, kAttrXorMappedAddress = 0x0020;
constexpr uint8_t kFamilyIpv4 = 0x01;

uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

// Unpredictable bytes for STUN transaction ids, WebSocket keys and frame masks.
void fill_random(uint8_t* out, size_t size) {
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  try {
    static std::random_device device;
    for (size_t i = 0; i < size; i += 4) {
      const uint32_t v = device();
      for (size_t k = 0; k < 4 && i + k < size; ++k) out[i + k] = (uint8_t)(v >> (8 * k));
    }
    return;
  } catch (...) {
  }
  static std::mt19937_64 fallback((uint64_t)Clock::now().time_since_epoch().count() ^ (uint64_t)(uintptr_t)&mutex);
  for (size_t i = 0; i < size; ++i) out[i] = (uint8_t)(fallback() >> 32);
}

std::string ipv4_text(uint32_t ip_be) {
  uint8_t b[4];
  std::memcpy(b, &ip_be, 4);
  char text[16];
  std::snprintf(text, sizeof text, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
  return text;
}

// Rust's u16::from_str: an optional '+', then one or more digits, at most 65535.
bool parse_u16(const std::string& text, uint16_t* out) {
  size_t i = !text.empty() && text[0] == '+' ? 1 : 0;
  if (i >= text.size()) return false;
  uint32_t value = 0;
  for (; i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    value = value * 10 + (uint32_t)(text[i] - '0');
    if (value > 65535) return false;
  }
  *out = (uint16_t)value;
  return true;
}

std::string json_string(const std::string& text) {
  // Invalid UTF-8 (a hand-edited user.json) becomes U+FFFD, as Rust's lossy conversion does.
  return json(text).dump(-1, ' ', false, json::error_handler_t::replace);
}

const json* member(const json& j, const char* key) {
  if (!j.is_object()) return nullptr;
  const auto it = j.find(key);
  return it == j.end() ? nullptr : &*it;
}

std::string lower(std::string text) {
  for (char& c : text) c = (char)std::tolower((unsigned char)c);
  return text;
}
std::string trim(const std::string& text) {
  const size_t start = text.find_first_not_of(" \t");
  if (start == std::string::npos) return {};
  return text.substr(start, text.find_last_not_of(" \t") - start + 1);
}
// True when the comma separated header value list holds token (case-insensitive).
bool has_token(const std::string& values, const char* token) {
  size_t start = 0;
  while (start <= values.size()) {
    size_t end = values.find(',', start);
    if (end == std::string::npos) end = values.size();
    if (lower(trim(values.substr(start, end - start))) == token) return true;
    start = end + 1;
  }
  return false;
}

bool valid_utf8(const std::string& text) {
  const auto* p = reinterpret_cast<const unsigned char*>(text.data());
  const size_t n = text.size();
  for (size_t i = 0; i < n;) {
    const unsigned char c = p[i];
    if (c < 0x80) { ++i; continue; }
    size_t len;
    uint32_t cp;
    if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
    else return false;
    if (i + len > n) return false;
    for (size_t k = 1; k < len; ++k) {
      if ((p[i + k] & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (p[i + k] & 0x3F);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += len;
  }
  return true;
}

std::string base64(const uint8_t* data, size_t size) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(4 * ((size + 2) / 3));
  for (size_t i = 0; i < size; i += 3) {
    const uint32_t bits = ((uint32_t)data[i] << 16) | (i + 1 < size ? (uint32_t)data[i + 1] << 8 : 0) | (i + 2 < size ? data[i + 2] : 0);
    out.push_back(alphabet[(bits >> 18) & 63]);
    out.push_back(alphabet[(bits >> 12) & 63]);
    out.push_back(i + 1 < size ? alphabet[(bits >> 6) & 63] : '=');
    out.push_back(i + 2 < size ? alphabet[bits & 63] : '=');
  }
  return out;
}

uint32_t rotl(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

// SHA-1, for Sec-WebSocket-Accept only.
void sha1(const uint8_t* data, size_t size, uint8_t digest[20]) {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  std::vector<uint8_t> m(data, data + size);
  const uint64_t bits = (uint64_t)size * 8;
  m.push_back(0x80);
  while (m.size() % 64 != 56) m.push_back(0);
  for (int i = 7; i >= 0; --i) m.push_back((uint8_t)(bits >> (8 * i)));
  for (size_t block = 0; block < m.size(); block += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = be32(&m[block + 4 * (size_t)i]);
    for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
      const uint32_t t = rotl(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rotl(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  for (int i = 0; i < 5; ++i) put32(digest + 4 * i, h[i]);
}

bool decode_address(const uint8_t* value, size_t size, bool xored, StunObservation* out) {
  if (size < 8 || value[1] != kFamilyIpv4) return false;
  uint16_t port = be16(value + 2);
  uint32_t ip = be32(value + 4);
  if (xored) {
    port ^= (uint16_t)(kMagicCookie >> 16);
    ip ^= kMagicCookie;
  }
  uint8_t octets[4];
  put32(octets, ip);
  out->answered = true;
  std::memcpy(&out->ip_be, octets, 4);
  out->port = port;
  return true;
}

// ------------------------------------------------------------------ URL
struct Url {
  bool secure = false;
  std::string host;        // without brackets or port
  std::string authority;   // host[:port] as written, for the Host header
  std::string path;        // path and query, at least "/"
  uint16_t port = 0;
};

bool printable(const std::string& text) {
  for (unsigned char c : text) if (c <= 0x20 || c >= 0x7F) return false;
  return true;
}

bool parse_url(const std::string& text, Url* out, std::string* error) {
  const size_t scheme_end = text.find("://");
  const std::string scheme = scheme_end == std::string::npos ? std::string() : lower(text.substr(0, scheme_end));
  if (scheme != "ws" && scheme != "wss") { *error = "the matchmaking URL is not ws:// or wss://"; return false; }
  Url url;
  url.secure = scheme == "wss";
  const size_t start = scheme_end + 3;
  const size_t end = text.find_first_of("/?#", start);
  std::string authority = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
  std::string path = end == std::string::npos ? std::string() : text.substr(end);
  path = path.substr(0, path.find('#'));
  if (path.empty() || path[0] != '/') path = "/" + path;
  const size_t at = authority.rfind('@');
  if (at != std::string::npos) authority = authority.substr(at + 1);
  std::string host = authority, port_text;
  if (!authority.empty() && authority[0] == '[') {
    const size_t close = authority.find(']');
    if (close == std::string::npos) { *error = "the matchmaking URL has a malformed host"; return false; }
    host = authority.substr(1, close - 1);
    if (close + 1 < authority.size()) {
      if (authority[close + 1] != ':') { *error = "the matchmaking URL has a malformed host"; return false; }
      port_text = authority.substr(close + 2);
    }
  } else {
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
      host = authority.substr(0, colon);
      port_text = authority.substr(colon + 1);
    }
  }
  if (host.empty() || !printable(authority) || !printable(path)) { *error = "the matchmaking URL has no usable host"; return false; }
  url.port = url.secure ? 443 : 80;
  if (!port_text.empty() && (!parse_u16(port_text, &url.port) || port_text[0] == '+')) {
    *error = "the matchmaking URL has an invalid port";
    return false;
  }
  url.host = host;
  url.authority = authority;
  url.path = path;
  *out = url;
  return true;
}

// ------------------------------------------------------------------ TCP and TLS
enum class Io { Ok, Timeout, Error, Cancelled };

#ifdef _WIN32
bool winsock_ready() {
  static const bool ready = [] {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ready;
}

std::string status_text(const char* what, SECURITY_STATUS status) {
  char text[96];
  std::snprintf(text, sizeof text, "%s failed (0x%08lX)", what, (unsigned long)status);
  return text;
}

// One TCP connection, with Schannel TLS on top for wss. Not thread-safe: the session's worker is
// its only user.
class Transport {
 public:
  Transport() = default;
  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;
  ~Transport() { close(); }

  // Connects to the first IPv4 address of url's host that answers, as client.rs does, then runs
  // the TLS handshake for wss. A failed handshake ends the attempt (no next address).
  Io open(const Url& url, const std::atomic<bool>& cancel, std::string* error) {
    if (!winsock_ready()) { *error = "networking is unavailable"; return Io::Error; }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* found = nullptr;
    const std::string port = std::to_string(url.port);
    const int resolved = getaddrinfo(url.host.c_str(), port.c_str(), &hints, &found);
    if (resolved != 0 || !found) {
      *error = "no IPv4 address for " + url.host + " (" + std::to_string(resolved) + ")";
      if (found) freeaddrinfo(found);
      return Io::Error;
    }
    std::vector<sockaddr_in> addresses;
    for (const addrinfo* a = found; a; a = a->ai_next)
      if (a->ai_family == AF_INET && a->ai_addrlen >= sizeof(sockaddr_in)) addresses.push_back(*reinterpret_cast<const sockaddr_in*>(a->ai_addr));
    freeaddrinfo(found);
    if (addresses.empty()) { *error = "no IPv4 address for " + url.host; return Io::Error; }
    std::string last = "no address answered";
    for (const sockaddr_in& address : addresses) {
      if (cancel.load()) return Io::Cancelled;
      const Io connected = connect_one(address, cancel, &last);
      if (connected == Io::Cancelled) return connected;
      if (connected != Io::Ok) continue;
      if (!url.secure) return Io::Ok;
      return tls_open(url.host, cancel, error);
    }
    *error = last;
    return Io::Error;
  }

  // Appends what arrives before the deadline: Ok once there is at least one byte.
  Io read(std::vector<uint8_t>& out, Clock::time_point deadline, const std::atomic<bool>* cancel) {
    if (!tls_) return raw_recv(out, deadline, cancel);
    for (;;) {
      if (!plain_.empty()) {
        out.insert(out.end(), plain_.begin(), plain_.end());
        plain_.clear();
        return Io::Ok;
      }
      if (!rx_.empty() && !need_more_) {
        SecBuffer buffers[4] = {{(unsigned long)rx_.size(), SECBUFFER_DATA, rx_.data()},
                                {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc desc{SECBUFFER_VERSION, 4, buffers};
        const SECURITY_STATUS status = DecryptMessage(&ctx_, &desc, 0, nullptr);
        if (status == SEC_E_INCOMPLETE_MESSAGE) { need_more_ = true; continue; }
        if (status != SEC_E_OK && status != SEC_I_RENEGOTIATE) return Io::Error;   // SEC_I_CONTEXT_EXPIRED: close_notify
        std::vector<uint8_t> extra;
        for (int i = 1; i < 4; ++i) {
          const auto* p = static_cast<const uint8_t*>(buffers[i].pvBuffer);
          if (!p || !buffers[i].cbBuffer) continue;
          if (buffers[i].BufferType == SECBUFFER_DATA) plain_.insert(plain_.end(), p, p + buffers[i].cbBuffer);
          else if (buffers[i].BufferType == SECBUFFER_EXTRA) extra.assign(p, p + buffers[i].cbBuffer);
        }
        rx_.swap(extra);
        if (status == SEC_I_RENEGOTIATE) {
          // TLS 1.3 post-handshake messages come this way: the rest goes back through the handshake.
          std::string ignored;
          if (handshake(false, nullptr, &ignored) != Io::Ok) return Io::Error;
        }
        continue;
      }
      const Io got = raw_recv(rx_, deadline, cancel);
      if (got != Io::Ok) return got;
      need_more_ = false;
    }
  }

  bool write(const uint8_t* data, size_t size) {
    if (!tls_) return raw_send(data, size);
    if (!sizes_.cbMaximumMessage) return false;
    while (size) {
      const size_t chunk = std::min<size_t>(size, sizes_.cbMaximumMessage);
      std::vector<uint8_t> record(sizes_.cbHeader + chunk + sizes_.cbTrailer);
      std::memcpy(record.data() + sizes_.cbHeader, data, chunk);
      SecBuffer buffers[4] = {{sizes_.cbHeader, SECBUFFER_STREAM_HEADER, record.data()},
                              {(unsigned long)chunk, SECBUFFER_DATA, record.data() + sizes_.cbHeader},
                              {sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, record.data() + sizes_.cbHeader + chunk},
                              {0, SECBUFFER_EMPTY, nullptr}};
      SecBufferDesc desc{SECBUFFER_VERSION, 4, buffers};
      if (EncryptMessage(&ctx_, 0, &desc, 0) != SEC_E_OK) return false;
      if (!raw_send(record.data(), (size_t)buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer)) return false;
      data += chunk;
      size -= chunk;
    }
    return true;
  }

  void close() {
    if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
    if (have_ctx_) { DeleteSecurityContext(&ctx_); have_ctx_ = false; }
    if (have_cred_) { FreeCredentialsHandle(&cred_); have_cred_ = false; }
    tls_ = false;
    need_more_ = false;
    rx_.clear();
    plain_.clear();
  }

 private:
  static constexpr unsigned long kContextFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                                                 ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;

  Io wait(bool for_write, Clock::time_point deadline, const std::atomic<bool>* cancel) const {
    for (;;) {
      if (cancel && cancel->load()) return Io::Cancelled;
      const auto now = Clock::now();
      if (now >= deadline) return Io::Timeout;
      long long left = std::chrono::duration_cast<milliseconds>(deadline - now).count();
      if (cancel && left > 100) left = 100;   // notice a cancel while connecting or upgrading
      if (left < 1) left = 1;
      fd_set ready, failed;
      FD_ZERO(&ready);
      FD_ZERO(&failed);
      FD_SET(sock_, &ready);
      FD_SET(sock_, &failed);
      timeval tv;
      tv.tv_sec = (long)(left / 1000);
      tv.tv_usec = (long)((left % 1000) * 1000);
      const int n = select(0, for_write ? nullptr : &ready, for_write ? &ready : nullptr, &failed, &tv);
      if (n == SOCKET_ERROR) return Io::Error;
      if (n > 0) return FD_ISSET(sock_, &ready) ? Io::Ok : Io::Error;
    }
  }

  Io connect_one(const sockaddr_in& address, const std::atomic<bool>& cancel, std::string* error) {
    close();
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &address.sin_addr, ip, sizeof ip);
    const std::string where = std::string("connecting to ") + ip + ":" + std::to_string(ntohs(address.sin_port));
    sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock_ == INVALID_SOCKET) { *error = where + ": no socket (" + std::to_string(WSAGetLastError()) + ")"; return Io::Error; }
    u_long nonblocking = 1;
    ioctlsocket(sock_, FIONBIO, &nonblocking);
    const BOOL nodelay = TRUE;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
    if (connect(sock_, reinterpret_cast<const sockaddr*>(&address), sizeof address) == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
      *error = where + ": error " + std::to_string(WSAGetLastError());
      close();
      return Io::Error;
    }
    const Io ready = wait(true, Clock::now() + kConnectTimeout, &cancel);
    if (ready == Io::Cancelled) { close(); return ready; }
    int so_error = 0;
    int length = sizeof so_error;
    getsockopt(sock_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &length);
    if (ready == Io::Ok && so_error == 0) return Io::Ok;
    *error = where + (ready == Io::Timeout ? std::string(": timed out") : ": error " + std::to_string(so_error));
    close();
    return Io::Error;
  }

  Io raw_recv(std::vector<uint8_t>& out, Clock::time_point deadline, const std::atomic<bool>* cancel) {
    for (;;) {
      if (sock_ == INVALID_SOCKET) return Io::Error;
      const Io ready = wait(false, deadline, cancel);
      if (ready != Io::Ok) return ready;
      uint8_t buffer[16384];
      const int n = recv(sock_, reinterpret_cast<char*>(buffer), (int)sizeof buffer, 0);
      if (n > 0) {
        out.insert(out.end(), buffer, buffer + n);
        return Io::Ok;
      }
      if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
      return Io::Error;   // closed by the peer, or failed
    }
  }

  bool raw_send(const uint8_t* data, size_t size) {
    const auto deadline = Clock::now() + kWriteTimeout;
    while (size) {
      if (sock_ == INVALID_SOCKET) return false;
      const int n = send(sock_, reinterpret_cast<const char*>(data), (int)std::min<size_t>(size, size_t(1) << 20), 0);
      if (n > 0) {
        data += n;
        size -= (size_t)n;
        continue;
      }
      if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
        if (wait(true, deadline, nullptr) != Io::Ok) return false;
        continue;
      }
      return false;
    }
    return true;
  }

  // Sends a handshake token SSPI allocated, and frees it.
  bool send_token(SecBuffer& token) {
    bool sent = true;
    if (token.pvBuffer) {
      if (token.cbBuffer) sent = raw_send(static_cast<const uint8_t*>(token.pvBuffer), token.cbBuffer);
      FreeContextBuffer(token.pvBuffer);
      token.pvBuffer = nullptr;
    }
    return sent;
  }

  Io tls_open(const std::string& host, const std::atomic<bool>& cancel, std::string* error) {
    // The system's protocol defaults and Schannel's own certificate and host name checks
    // (SCH_CRED_AUTO_CRED_VALIDATION against host as the target name).
    SCHANNEL_CRED credentials{};
    credentials.dwVersion = SCHANNEL_CRED_VERSION;
    credentials.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
    const SECURITY_STATUS status = AcquireCredentialsHandleA(nullptr, const_cast<LPSTR>(UNISP_NAME_A), SECPKG_CRED_OUTBOUND, nullptr,
                                                             &credentials, nullptr, nullptr, &cred_, nullptr);
    if (status != SEC_E_OK) { *error = status_text("TLS setup", status); return Io::Error; }
    have_cred_ = true;
    host_ = host;
    tls_ = true;
    const Io done = handshake(true, &cancel, error);
    if (done != Io::Ok) return done;
    if (QueryContextAttributesA(&ctx_, SECPKG_ATTR_STREAM_SIZES, &sizes_) != SEC_E_OK || !sizes_.cbMaximumMessage) {
      *error = "TLS stream sizes unavailable";
      return Io::Error;
    }
    return Io::Ok;
  }

  // The client side of the TLS handshake. initial: the first flight; otherwise rx_ holds what
  // DecryptMessage handed back with SEC_I_RENEGOTIATE.
  Io handshake(bool initial, const std::atomic<bool>* cancel, std::string* error) {
    unsigned long attributes = 0;
    SEC_CHAR* target = const_cast<SEC_CHAR*>(host_.c_str());
    if (initial) {
      SecBuffer out{0, SECBUFFER_TOKEN, nullptr};
      SecBufferDesc out_desc{SECBUFFER_VERSION, 1, &out};
      const SECURITY_STATUS status = InitializeSecurityContextA(&cred_, nullptr, target, kContextFlags, 0, 0, nullptr, 0, &ctx_,
                                                                &out_desc, &attributes, nullptr);
      if (status != SEC_I_CONTINUE_NEEDED) {
        if (out.pvBuffer) FreeContextBuffer(out.pvBuffer);
        *error = status_text("TLS handshake", status);
        return Io::Error;
      }
      have_ctx_ = true;
      if (!send_token(out)) { *error = "sending the TLS hello failed"; return Io::Error; }
    }
    bool read_more = rx_.empty();
    for (;;) {
      if (read_more) {
        const Io got = raw_recv(rx_, Clock::now() + kHandshakeReadTimeout, cancel);
        if (got == Io::Cancelled) return got;
        if (got != Io::Ok) {
          *error = got == Io::Timeout ? "the TLS handshake timed out" : "the connection closed during the TLS handshake";
          return Io::Error;
        }
      }
      SecBuffer in[2] = {{(unsigned long)rx_.size(), SECBUFFER_TOKEN, rx_.data()}, {0, SECBUFFER_EMPTY, nullptr}};
      SecBufferDesc in_desc{SECBUFFER_VERSION, 2, in};
      SecBuffer out{0, SECBUFFER_TOKEN, nullptr};
      SecBufferDesc out_desc{SECBUFFER_VERSION, 1, &out};
      const SECURITY_STATUS status = InitializeSecurityContextA(&cred_, &ctx_, target, kContextFlags, 0, 0, &in_desc, 0, nullptr,
                                                                &out_desc, &attributes, nullptr);
      if (status == SEC_E_INCOMPLETE_MESSAGE) {
        if (out.pvBuffer) FreeContextBuffer(out.pvBuffer);
        read_more = true;
        continue;
      }
      // A token goes out on success, and on a failure with an extended error (the TLS alert).
      const bool sent = send_token(out);
      if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED) { *error = status_text("TLS handshake", status); return Io::Error; }
      if (!sent) { *error = "sending a TLS handshake message failed"; return Io::Error; }
      if (in[1].BufferType == SECBUFFER_EXTRA && in[1].cbBuffer > 0 && in[1].cbBuffer <= rx_.size()) {
        rx_.erase(rx_.begin(), rx_.end() - (std::ptrdiff_t)in[1].cbBuffer);
        read_more = false;
      } else {
        rx_.clear();
        read_more = true;
      }
      if (status == SEC_E_OK) return Io::Ok;   // anything left in rx_ is application data
    }
  }

  SOCKET sock_ = INVALID_SOCKET;
  bool tls_ = false;
  CredHandle cred_{};
  bool have_cred_ = false;
  CtxtHandle ctx_{};
  bool have_ctx_ = false;
  SecPkgContext_StreamSizes sizes_{};
  std::string host_;
  std::vector<uint8_t> rx_;      // received TLS records not yet decrypted
  std::vector<uint8_t> plain_;   // decrypted, not yet handed out
  bool need_more_ = false;       // rx_ holds only part of a record
};
#else
// libcurl connect-only: curl resolves (IPv4 only), connects, and runs TLS with its usual
// certificate checks; the WebSocket bytes then go through curl_easy_send and curl_easy_recv.
class Transport {
 public:
  Transport() = default;
  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;
  ~Transport() { close(); }

  Io open(const Url& url, const std::atomic<bool>& cancel, std::string* error) {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    close();
    curl_ = curl_easy_init();
    if (!curl_) { *error = "networking is unavailable"; return Io::Error; }
    const std::string target = std::string(url.secure ? "https://" : "http://") + url.host + ":" + std::to_string(url.port) + "/";
    char message[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl_, CURLOPT_URL, target.c_str());
    curl_easy_setopt(curl_, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(curl_, CURLOPT_IPRESOLVE, (long)CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl_, CURLOPT_PROXY, "");   // straight to the service, as client.rs connects
    curl_easy_setopt(curl_, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);   // ALPN must not pick HTTP/2
    curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, (long)kConnectTimeout.count());
    curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl_, CURLOPT_XFERINFOFUNCTION, &Transport::progress);
    curl_easy_setopt(curl_, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool>*>(&cancel));
    curl_easy_setopt(curl_, CURLOPT_ERRORBUFFER, message);
    const CURLcode code = curl_easy_perform(curl_);
    curl_easy_setopt(curl_, CURLOPT_ERRORBUFFER, (char*)nullptr);
    curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 1L);
    if (cancel.load()) return Io::Cancelled;
    if (code != CURLE_OK) {
      *error = "connecting to " + url.host + ": " + (message[0] ? std::string(message) : std::string(curl_easy_strerror(code)));
      return Io::Error;
    }
    curl_socket_t fd = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(curl_, CURLINFO_ACTIVESOCKET, &fd) != CURLE_OK || fd == CURL_SOCKET_BAD) {
      *error = "connecting to " + url.host + ": no socket";
      return Io::Error;
    }
    fd_ = fd;
    return Io::Ok;
  }

  Io read(std::vector<uint8_t>& out, Clock::time_point deadline, const std::atomic<bool>* cancel) {
    if (!curl_ || fd_ == CURL_SOCKET_BAD) return Io::Error;
    for (;;) {
      if (cancel && cancel->load()) return Io::Cancelled;
      // Ask curl first: TLS may already hold decrypted bytes the socket no longer shows.
      uint8_t buffer[16384];
      size_t got = 0;
      const CURLcode code = curl_easy_recv(curl_, buffer, sizeof buffer, &got);
      if (code == CURLE_OK) {
        if (!got) return Io::Error;   // closed by the peer
        out.insert(out.end(), buffer, buffer + got);
        return Io::Ok;
      }
      if (code != CURLE_AGAIN) return Io::Error;
      const Io ready = wait(POLLIN, deadline, cancel);
      if (ready != Io::Ok) return ready;
    }
  }

  bool write(const uint8_t* data, size_t size) {
    if (!curl_ || fd_ == CURL_SOCKET_BAD) return false;
    const auto deadline = Clock::now() + kWriteTimeout;
    while (size) {
      size_t sent = 0;
      const CURLcode code = curl_easy_send(curl_, data, size, &sent);
      if (code != CURLE_OK && code != CURLE_AGAIN) return false;
      data += sent;
      size -= sent;
      if (size && !sent && wait(POLLOUT, deadline, nullptr) != Io::Ok) return false;
    }
    return true;
  }

  void close() {
    if (curl_) { curl_easy_cleanup(curl_); curl_ = nullptr; }
    fd_ = CURL_SOCKET_BAD;
  }

 private:
  static int progress(void* cancel, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<const std::atomic<bool>*>(cancel)->load() ? 1 : 0;
  }

  Io wait(short events, Clock::time_point deadline, const std::atomic<bool>* cancel) const {
    for (;;) {
      if (cancel && cancel->load()) return Io::Cancelled;
      const auto now = Clock::now();
      if (now >= deadline) return Io::Timeout;
      long long left = std::chrono::duration_cast<milliseconds>(deadline - now).count();
      if (cancel && left > 100) left = 100;
      if (left < 1) left = 1;
      pollfd p{fd_, events, 0};
      const int n = poll(&p, 1, (int)left);
      if (n > 0) return (p.revents & POLLNVAL) ? Io::Error : Io::Ok;   // a hang-up shows in the next recv
      if (n < 0 && errno != EINTR) return Io::Error;
    }
  }

  CURL* curl_ = nullptr;
  curl_socket_t fd_ = CURL_SOCKET_BAD;
};
#endif

// ------------------------------------------------------------------ WebSocket (RFC 6455, client side)
class WebSocket {
 public:
  enum class Msg { Text, Other, Close, Timeout, Error };

  // TCP (and TLS), then the HTTP upgrade, each step with client.rs's limits.
  Io open(const Url& url, const std::atomic<bool>& cancel, std::string* error) {
    const Io connected = transport_.open(url, cancel, error);
    if (connected != Io::Ok) return connected;
    return upgrade(url, cancel, error);
  }

  bool send_text(const std::string& text) { return send_frame(0x1, text.data(), text.size()); }
  // An empty close frame, as tungstenite's close(None) sends; once per connection.
  void send_close() {
    if (close_sent_) return;
    close_sent_ = true;
    send_frame(0x8, nullptr, 0);
  }

  // Waits up to timeout for one whole message. Text: *text holds it (valid UTF-8). Other: binary,
  // ping or pong (a ping is answered). Close: the service closed. Error: the connection is unusable.
  Msg read(std::string* text, milliseconds timeout) {
    if (broken_) return Msg::Error;
    const auto deadline = Clock::now() + timeout;
    for (;;) {
      uint8_t opcode = 0;
      bool fin = false;
      std::string payload;
      const Frame frame = take_frame(&opcode, &fin, &payload);
      if (frame == Frame::Bad) { broken_ = true; return Msg::Error; }
      if (frame == Frame::Ok) {
        if (opcode >= 0x8) {
          if (!fin || payload.size() > 125) { broken_ = true; return Msg::Error; }
          if (opcode == 0x8) return Msg::Close;
          if (opcode == 0x9) { send_frame(0xA, payload.data(), payload.size()); return Msg::Other; }
          if (opcode == 0xA) return Msg::Other;
          broken_ = true;
          return Msg::Error;
        }
        if (opcode == 0x0) {
          if (!message_opcode_) { broken_ = true; return Msg::Error; }
        } else if (opcode == 0x1 || opcode == 0x2) {
          if (message_opcode_) { broken_ = true; return Msg::Error; }
          message_opcode_ = opcode;
          message_.clear();
        } else {
          broken_ = true;
          return Msg::Error;
        }
        if (message_.size() + payload.size() > kMaxMessage) { broken_ = true; return Msg::Error; }
        message_ += payload;
        if (!fin) continue;
        const uint8_t done = message_opcode_;
        message_opcode_ = 0;
        if (done == 0x2) { message_.clear(); return Msg::Other; }
        if (!valid_utf8(message_)) { broken_ = true; return Msg::Error; }
        *text = std::move(message_);
        message_.clear();
        return Msg::Text;
      }
      const Io got = transport_.read(in_, deadline, nullptr);
      if (got == Io::Timeout) return Msg::Timeout;
      if (got != Io::Ok) { broken_ = true; return Msg::Error; }
    }
  }

 private:
  enum class Frame { NeedMore, Ok, Bad };

  Io upgrade(const Url& url, const std::atomic<bool>& cancel, std::string* error) {
    uint8_t nonce[16];
    fill_random(nonce, sizeof nonce);
    const std::string key = base64(nonce, sizeof nonce);
    const std::string request = "GET " + url.path + " HTTP/1.1\r\nHost: " + url.authority +
                                "\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + key +
                                "\r\n\r\n";
    if (!transport_.write(reinterpret_cast<const uint8_t*>(request.data()), request.size())) {
      *error = "sending the upgrade request failed";
      return Io::Error;
    }
    static const char kEnd[] = "\r\n\r\n";
    std::vector<uint8_t> response;
    size_t end = 0;
    for (;;) {
      const auto found = std::search(response.begin(), response.end(), kEnd, kEnd + 4);
      if (found != response.end()) { end = (size_t)(found - response.begin()); break; }
      if (response.size() > 65536) { *error = "the upgrade response is too large"; return Io::Error; }
      const Io got = transport_.read(response, Clock::now() + kHandshakeReadTimeout, &cancel);
      if (got == Io::Cancelled) return got;
      if (got != Io::Ok) {
        *error = got == Io::Timeout ? "timed out waiting for the upgrade response" : "the connection closed during the upgrade";
        return Io::Error;
      }
    }
    const std::string head(response.begin(), response.begin() + (std::ptrdiff_t)end);
    in_.assign(response.begin() + (std::ptrdiff_t)end + 4, response.end());   // frames that came with it
    in_pos_ = 0;

    size_t line_end = head.find("\r\n");
    if (line_end == std::string::npos) line_end = head.size();
    const std::string status_line = head.substr(0, line_end);
    if (status_line.size() < 12 || status_line.compare(0, 9, "HTTP/1.1 ") != 0 || !std::isdigit((unsigned char)status_line[9]) ||
        !std::isdigit((unsigned char)status_line[10]) || !std::isdigit((unsigned char)status_line[11])) {
      *error = "the upgrade response is not HTTP/1.1";
      return Io::Error;
    }
    const int status = std::atoi(status_line.substr(9, 3).c_str());
    if (status != 101) { *error = "HTTP error: " + std::to_string(status); return Io::Error; }
    std::string accept, upgrade_values, connection_values;
    for (size_t pos = line_end + 2; pos < head.size();) {
      size_t next = head.find("\r\n", pos);
      if (next == std::string::npos) next = head.size();
      const std::string line = head.substr(pos, next - pos);
      pos = next + 2;
      const size_t colon = line.find(':');
      if (colon == std::string::npos) { *error = "the upgrade response has a malformed header"; return Io::Error; }
      const std::string name = lower(trim(line.substr(0, colon)));
      const std::string value = trim(line.substr(colon + 1));
      if (name == "sec-websocket-accept") accept = value;
      else if (name == "upgrade") upgrade_values += value + ",";
      else if (name == "connection") connection_values += value + ",";
      else if (name == "sec-websocket-extensions" && !value.empty()) { *error = "the server chose a WebSocket extension"; return Io::Error; }
    }
    if (!has_token(upgrade_values, "websocket") || !has_token(connection_values, "upgrade")) {
      *error = "the server did not upgrade to a WebSocket";
      return Io::Error;
    }
    if (accept != detail::websocket_accept_key(key)) { *error = "the server's Sec-WebSocket-Accept does not match"; return Io::Error; }
    return Io::Ok;
  }

  // One frame off the front of in_.
  Frame take_frame(uint8_t* opcode, bool* fin, std::string* payload) {
    const size_t available = in_.size() - in_pos_;
    const uint8_t* p = in_.data() + in_pos_;
    if (available < 2) return Frame::NeedMore;
    if (p[0] & 0x70) return Frame::Bad;   // no extension was negotiated
    if (p[1] & 0x80) return Frame::Bad;   // a server never masks
    uint64_t length = p[1] & 0x7F;
    size_t header = 2;
    if (length == 126) {
      if (available < 4) return Frame::NeedMore;
      length = be16(p + 2);
      header = 4;
    } else if (length == 127) {
      if (available < 10) return Frame::NeedMore;
      length = ((uint64_t)be32(p + 2) << 32) | be32(p + 6);
      header = 10;
    }
    if (length > kMaxMessage) return Frame::Bad;
    if (available - header < length) return Frame::NeedMore;
    *opcode = p[0] & 0x0F;
    *fin = (p[0] & 0x80) != 0;
    payload->assign(reinterpret_cast<const char*>(p + header), (size_t)length);
    in_pos_ += header + (size_t)length;
    if (in_pos_ == in_.size()) {
      in_.clear();
      in_pos_ = 0;
    } else if (in_pos_ >= 65536) {
      in_.erase(in_.begin(), in_.begin() + (std::ptrdiff_t)in_pos_);
      in_pos_ = 0;
    }
    return Frame::Ok;
  }

  bool send_frame(uint8_t opcode, const void* data, size_t size) {
    if (broken_) return false;
    std::vector<uint8_t> frame;
    frame.reserve(size + 14);
    frame.push_back((uint8_t)(0x80 | opcode));
    if (size < 126) {
      frame.push_back((uint8_t)(0x80 | size));
    } else if (size <= 0xFFFF) {
      frame.push_back(0x80 | 126);
      frame.push_back((uint8_t)(size >> 8));
      frame.push_back((uint8_t)size);
    } else {
      frame.push_back(0x80 | 127);
      for (int shift = 56; shift >= 0; shift -= 8) frame.push_back((uint8_t)((uint64_t)size >> shift));
    }
    uint8_t mask[4];
    fill_random(mask, sizeof mask);
    frame.insert(frame.end(), mask, mask + 4);
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) frame.push_back((uint8_t)(bytes[i] ^ mask[i & 3]));
    if (!transport_.write(frame.data(), frame.size())) {
      broken_ = true;
      return false;
    }
    return true;
  }

  Transport transport_;
  std::vector<uint8_t> in_;   // received, not yet decoded
  size_t in_pos_ = 0;
  std::string message_;       // a fragmented message being put together
  uint8_t message_opcode_ = 0;
  bool close_sent_ = false;
  bool broken_ = false;
};

}  // namespace

// ------------------------------------------------------------------ the session
namespace detail {
struct SessionShared {
  std::atomic<uint8_t> state{(uint8_t)State::Connecting};
  std::atomic<bool> cancel{false};
  mutable std::mutex mutex;
  bool has_result = false;
  std::string result_json, error, latest_version;

  void set_state(State s) { state.store((uint8_t)s); }
  bool cancelled() const { return cancel.load(); }
  // The text is stored before the state that announces it.
  void fail(const std::string& message) {
    host::log("slippi: mm service: %s", message.c_str());
    {
      std::lock_guard<std::mutex> lock(mutex);
      error = message;
    }
    set_state(State::Failed);
  }
};
}  // namespace detail

namespace {

// client.rs Session: the worker that owns the connection. It touches nothing but its own copies
// and the shared state, so the Session can go away while it winds down.
class Worker {
 public:
  Worker(std::shared_ptr<detail::SessionShared> shared, std::string url, std::string join)
      : shared_(std::move(shared)), url_(std::move(url)), join_(std::move(join)) {}

  void run() noexcept {
    try {
      loop();
    } catch (const std::exception& e) {
      give_up_on_exception(e.what());
    } catch (...) {
      give_up_on_exception("unknown");
    }
  }

 private:
  enum class Ended { Done, Lost, JoinTimeout };

  void give_up_on_exception(const char* what) noexcept {
    try {
      host::log("slippi: mm service: stopped on an error (%s)", what);
      const State s = (State)shared_->state.load();
      if (s == State::Connecting || s == State::Queued) {
        if (shared_->cancelled()) shared_->set_state(State::Idle);
        else shared_->fail(ticket_ ? kLostConnection : kFailedConnect);
      }
    } catch (...) {
      shared_->set_state(State::Failed);
    }
  }

  void loop() {
    int connect_failures = 0;
    for (;;) {
      if (shared_->cancelled()) { shared_->set_state(State::Idle); return; }

      WebSocket socket;
      std::string error;
      Url url;
      const Io opened = parse_url(url_, &url, &error) ? socket.open(url, shared_->cancel, &error) : Io::Error;
      if (opened == Io::Cancelled) { shared_->set_state(State::Idle); return; }
      if (opened != Io::Ok) {
        host::log("slippi: mm service: connect failed: %s", error.c_str());
        if (give_up(++connect_failures) || sleep_unless_cancelled(kRetryDelay)) return;
        continue;
      }
      connect_failures = 0;

      const std::string first = ticket_ ? resume_json(ticket_->first, ticket_->second) : join_;
      if (!socket.send_text(first)) {
        host::log("slippi: mm service: failed to send the first message");
        socket.send_close();
        if (give_up(++connect_failures) || sleep_unless_cancelled(kRetryDelay)) return;
        continue;
      }

      switch (serve(socket)) {
        case Ended::Done:
          return;
        case Ended::JoinTimeout:
          shared_->fail(kFailedJoin);
          return;
        case Ended::Lost:
          if (!ticket_) { shared_->fail(kLostConnection); return; }
          if (!resume_deadline_) {
            resume_deadline_ = Clock::now() + kResumeWindow;
          } else if (Clock::now() >= *resume_deadline_) {
            shared_->fail(kLostConnection);
            return;
          }
          host::log("slippi: mm service: connection lost, resuming the ticket");
          if (sleep_unless_cancelled(kRetryDelay)) return;
          break;
      }
    }
  }

  // Whether a failed connect ends the session: past the resume window when resuming, after a few
  // attempts when first joining. Fails the session when it does.
  bool give_up(int connect_failures) {
    const bool done = resume_deadline_ ? Clock::now() >= *resume_deadline_ : connect_failures >= kConnectAttempts;
    if (done) shared_->fail(ticket_ ? kLostConnection : kFailedConnect);
    return done;
  }

  // One connection, until the session ends or the connection drops.
  Ended serve(WebSocket& socket) {
    const auto opened = Clock::now();
    auto last_frame = opened;
    bool pinged = false;
    acknowledged_ = false;
    for (;;) {
      if (shared_->cancelled()) {
        socket.send_text(simple_json("cancel"));
        socket.send_close();
        shared_->set_state(State::Idle);
        return Ended::Done;
      }

      std::string text;
      switch (socket.read(&text, kPollReadTimeout)) {
        case WebSocket::Msg::Text: {
          last_frame = Clock::now();
          pinged = false;
          const std::optional<Ended> ended = handle(text, socket);
          if (ended) return *ended;
          break;
        }
        case WebSocket::Msg::Other:   // binary, ping or pong: nothing to read, but the service is there
          last_frame = Clock::now();
          pinged = false;
          break;
        case WebSocket::Msg::Close:
          host::log("slippi: mm service: the service closed the connection");
          return Ended::Lost;
        case WebSocket::Msg::Timeout:
          break;
        case WebSocket::Msg::Error:
          host::log("slippi: mm service: read failed");
          return Ended::Lost;
      }

      if (!acknowledged_ && Clock::now() - opened >= kJoinTimeout) {
        host::log("slippi: mm service: the service did not acknowledge the connection in time");
        socket.send_close();
        return ticket_ ? Ended::Lost : Ended::JoinTimeout;
      }
      const auto idle = Clock::now() - last_frame;
      if (idle >= kLivenessTimeout) {
        host::log("slippi: mm service: nothing from the service for %lld ms, connection lost",
                  (long long)std::chrono::duration_cast<milliseconds>(idle).count());
        socket.send_close();
        return Ended::Lost;
      }
      if (idle >= kPingAfter && !pinged) {
        pinged = true;
        socket.send_text(simple_json("ping"));
      }
    }
  }

  // One message from the service. A value when the connection is done.
  std::optional<Ended> handle(const std::string& text, WebSocket& socket) {
    const json message = json::parse(text, nullptr, false);
    const json* type = member(message, "type");
    if (!type || !type->is_string()) {
      host::log("slippi: mm service: ignoring a malformed message");
      return std::nullopt;
    }
    const std::string& kind = type->get_ref<const std::string&>();

    if (kind == "joined") {
      const json* ticket_id = member(message, "ticketId");
      const json* token = member(message, "token");
      if (ticket_id && ticket_id->is_string() && token && token->is_string()) {
        ticket_ = std::make_pair(ticket_id->get<std::string>(), token->get<std::string>());
        host::log("slippi: mm service: ticket %s accepted", ticket_->first.c_str());
        acknowledged_ = true;
        // The service took this connection, so the next drop gets a whole resume window.
        resume_deadline_.reset();
        shared_->set_state(State::Queued);
      }
      return std::nullopt;
    }
    if (kind == "status") return std::nullopt;
    if (kind == "match") {
      const json* match_id = member(message, "matchId");
      const json* is_host = member(message, "isHost");
      if (match_id && match_id->is_string() && (!is_host || is_host->is_boolean()))
        host::log("slippi: mm service: match found: %s (host: %s)", match_id->get_ref<const std::string&>().c_str(),
                  is_host && is_host->get<bool>() ? "true" : "false");
      else
        host::log("slippi: mm service: the match message is unreadable");
      {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->result_json = text;
        shared_->has_result = true;
      }
      shared_->set_state(State::Matched);
      socket.send_close();
      return Ended::Done;
    }
    if (kind == "error") {
      std::string reason = kServerError;
      const json* error = member(message, "error");
      const json* latest = member(message, "latestVersion");
      if (error && error->is_string() && (!latest || latest->is_string())) {
        reason = error->get<std::string>();
        if (latest && !latest->get_ref<const std::string&>().empty()) {
          // The service knows a newer build exists: kept for players whose update check is broken.
          host::log("slippi: mm service: the service names Slippi %s as the latest", latest->get_ref<const std::string&>().c_str());
          std::lock_guard<std::mutex> lock(shared_->mutex);
          shared_->latest_version = latest->get<std::string>();
        }
      }
      shared_->fail(reason);
      socket.send_close();
      return Ended::Done;
    }
    return std::nullopt;
  }

  // Sleeps in small steps so a cancel is noticed. True if cancelled.
  bool sleep_unless_cancelled(milliseconds total) {
    const auto start = Clock::now();
    while (Clock::now() - start < total) {
      if (shared_->cancelled()) { shared_->set_state(State::Idle); return true; }
      std::this_thread::sleep_for(milliseconds(50));
    }
    return false;
  }

  std::shared_ptr<detail::SessionShared> shared_;
  const std::string url_;
  const std::string join_;
  std::optional<std::pair<std::string, std::string>> ticket_;   // ticket id and token once accepted
  std::optional<Clock::time_point> resume_deadline_;            // set when a queued ticket's connection drops
  bool acknowledged_ = false;                                   // this connection's join or resume was answered
};

}  // namespace

// ------------------------------------------------------------------ public API
void stun_build_request(uint8_t out[kStunRequestLen]) {
  if (!out) return;
  put16(out, kBindingRequest);
  put16(out + 2, 0);
  put32(out + 4, kMagicCookie);
  fill_random(out + 8, 12);
}

bool stun_parse_response(const uint8_t request[kStunRequestLen], const uint8_t* resp, size_t len, StunObservation* out) {
  if (!request || !resp || !out || len < kStunRequestLen) return false;
  if (be16(resp) != kBindingSuccess) return false;
  const size_t length = be16(resp + 2);
  if (be32(resp + 4) != kMagicCookie || std::memcmp(resp + 8, request + 8, 12) != 0) return false;
  const uint8_t* body = resp + kStunRequestLen;
  const size_t body_len = std::min(len, kStunRequestLen + length) - kStunRequestLen;

  StunObservation fallback;
  bool have_fallback = false;
  size_t offset = 0;
  while (offset + 4 <= body_len) {
    const uint16_t attr_type = be16(body + offset);
    const size_t attr_len = be16(body + offset + 2);
    const size_t value_start = offset + 4;
    const size_t value_end = value_start + attr_len;
    if (value_end > body_len) break;
    if (attr_type == kAttrXorMappedAddress) {
      StunObservation found;
      if (decode_address(body + value_start, attr_len, true, &found)) {
        *out = found;
        return true;
      }
    } else if (attr_type == kAttrMappedAddress && !have_fallback) {
      have_fallback = decode_address(body + value_start, attr_len, false, &fallback);
    }
    offset = value_end + ((4 - (attr_len % 4)) % 4);   // attributes are padded to four bytes
  }
  if (!have_fallback) return false;
  *out = fallback;
  return true;
}

const char* classify_nat(const StunObservation& a, const StunObservation& b) {
  if (!a.answered || !b.answered) return "unknown";
  return a.ip_be == b.ip_be && a.port == b.port ? "cone" : "symmetric";
}

Join build_join(const Request& request) {
  Join join;
  join.uid = request.uid;
  join.play_key = request.play_key;
  join.connect_code = request.connect_code_self;
  join.display_name = request.display_name;
  join.mode = request.mode;
  join.search_connect_code = request.connect_code;
  join.app_version = request.app_version;
  join.nat_type = classify_nat(request.stun[0], request.stun[1]);
  join.port = request.netplay_port;
  const StunObservation* reflexive = request.stun[0].answered ? &request.stun[0] : request.stun[1].answered ? &request.stun[1] : nullptr;
  if (reflexive) {
    join.candidates.push_back(Candidate{ipv4_text(reflexive->ip_be), reflexive->port, "srflx"});
    join.port = reflexive->port;
  }
  const size_t colon = request.lan_addr.rfind(':');
  uint16_t lan_port = 0;
  if (colon != std::string::npos && parse_u16(request.lan_addr.substr(colon + 1), &lan_port))
    join.candidates.push_back(Candidate{request.lan_addr.substr(0, colon), lan_port, "host"});
  join.lan_addr = request.lan_addr;
  return join;
}

std::string join_json(const Join& join) {
  std::string s = "{\"type\":\"join\",\"user\":{\"uid\":" + json_string(join.uid) + ",\"playKey\":" + json_string(join.play_key) +
                  ",\"connectCode\":" + json_string(join.connect_code) + ",\"displayName\":" + json_string(join.display_name) +
                  "},\"search\":{\"mode\":" + std::to_string((unsigned)join.mode) + ",\"connectCode\":[";
  for (size_t i = 0; i < join.search_connect_code.size(); ++i) {
    if (i) s += ',';
    s += std::to_string((unsigned)join.search_connect_code[i]);
  }
  s += "]},\"appVersion\":" + json_string(join.app_version) + ",\"port\":" + std::to_string((unsigned)join.port) +
       ",\"ipAddressLan\":" + json_string(join.lan_addr);
  if (!join.candidates.empty()) {
    s += ",\"candidates\":[";
    for (size_t i = 0; i < join.candidates.size(); ++i) {
      const Candidate& c = join.candidates[i];
      if (i) s += ',';
      s += "{\"ip\":" + json_string(c.ip) + ",\"port\":" + std::to_string((unsigned)c.port) + ",\"type\":" + json_string(c.type) + "}";
    }
    s += "]";
  }
  if (!join.nat_type.empty()) s += ",\"natType\":" + json_string(join.nat_type);
  s += "}";
  return s;
}

std::string resume_json(const std::string& ticket_id, const std::string& token) {
  return "{\"type\":\"resume\",\"ticketId\":" + json_string(ticket_id) + ",\"token\":" + json_string(token) + "}";
}

std::string simple_json(const char* type) { return "{\"type\":" + json_string(type ? type : "") + "}"; }

std::string default_url(const std::string& app_version) {
  const char* override_url = host::test_env::getenv(kUrlEnvVar);
  if (override_url && *override_url) return override_url;
  return app_version.find("dev") != std::string::npos ? kDevUrl : kProdUrl;
}

Session::Session(Request request, std::string url) : shared_(std::make_shared<detail::SessionShared>()) {
  if (url.empty()) url = default_url(request.app_version);
  const Join join = build_join(request);
  // What two servers saw decides the NAT type; the addresses themselves stay out of the log.
  host::log("slippi: mm service: NAT type: %s", join.nat_type.c_str());
  host::log("slippi: mm service: starting a session against %s", url.c_str());
  shared_->set_state(State::Connecting);
  try {
    // Never joined: a cancel must not wait on a connect or handshake in progress.
    std::thread([shared = shared_, url, text = join_json(join)]() mutable {
      Worker(std::move(shared), std::move(url), std::move(text)).run();
    }).detach();
  } catch (...) {
    shared_->fail(kFailedConnect);
  }
}

Session::~Session() { cancel(); }

State Session::state() const { return (State)shared_->state.load(); }

bool Session::take_result_json(std::string* out) {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  if (!out || !shared_->has_result) return false;
  *out = std::move(shared_->result_json);
  shared_->result_json.clear();
  shared_->has_result = false;
  return true;
}

std::string Session::error_message() const {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  return shared_->error;
}

std::string Session::latest_version() const {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  return shared_->latest_version;
}

void Session::cancel() { shared_->cancel.store(true); }

namespace detail {
std::string websocket_accept_key(const std::string& key) {
  const std::string text = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t digest[20];
  sha1(reinterpret_cast<const uint8_t*>(text.data()), text.size(), digest);
  return base64(digest, sizeof digest);
}

std::string probe(const std::string& text) {
  try {
    Url url;
    std::string error;
    if (!parse_url(text, &url, &error)) return error;
    std::atomic<bool> never{false};
    WebSocket socket;
    const Io opened = socket.open(url, never, &error);
    if (opened != Io::Ok) return error.empty() ? std::string("failed") : error;
    socket.send_close();
    return {};
  } catch (const std::exception& e) {
    return e.what();
  } catch (...) {
    return "failed";
  }
}
}  // namespace detail

}  // namespace slippi::mm_service
