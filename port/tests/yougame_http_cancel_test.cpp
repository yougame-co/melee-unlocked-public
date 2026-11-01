// Real libcurl with connected socketpair fakes: no bind, DNS or network connection.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "winhttp_cancel.h"
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

struct FakeSocket {
  int fds[2];
  std::promise<void> opened;
  FakeSocket() { CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0); }
  ~FakeSocket() { if (fds[0] >= 0) ::close(fds[0]); ::close(fds[1]); }
  static curl_socket_t open(void* data, curlsocktype, curl_sockaddr*) {
    auto& fake = *static_cast<FakeSocket*>(data);
    const int fd = fake.fds[0];
    if (fd < 0) return CURL_SOCKET_BAD;
    fake.fds[0] = -1; // curl owns this descriptor from here
    fake.opened.set_value();
    return fd;
  }
  static int connected(void*, curl_socket_t, curlsocktype) { return CURL_SOCKOPT_ALREADY_CONNECTED; }
  CURL* request() {
    CURL* easy = curl_easy_init(); CHECK(easy);
    curl_easy_setopt(easy, CURLOPT_URL, "http://127.0.0.1:1/");
    curl_easy_setopt(easy, CURLOPT_PROXY, "");
    curl_easy_setopt(easy, CURLOPT_OPENSOCKETFUNCTION, open);
    curl_easy_setopt(easy, CURLOPT_OPENSOCKETDATA, this);
    curl_easy_setopt(easy, CURLOPT_SOCKOPTFUNCTION, connected); // skips connect(), uses our socketpair
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 5000L);
    return easy;
  }
};
void retained_connect_only() {
  FakeSocket fake;
  CHECK(::send(fake.fds[1], "ready", 5, 0) == 5);
  CURL* easy = fake.request();
  curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 1L);
  CURLM* multi = nullptr;
  CHECK(winhttp_perform(easy, [] { return false; }, &multi) == CURLE_OK);
  CHECK(multi);
  char bytes[5]; size_t got = 0;
  CHECK(curl_easy_recv(easy, bytes, sizeof bytes, &got) == CURLE_OK);
  CHECK(got == 5 && std::string(bytes, got) == "ready");
  curl_multi_remove_handle(multi, easy); curl_multi_cleanup(multi); curl_easy_cleanup(easy);
}
void cancel_slow_response() {
  FakeSocket fake;
  std::atomic<bool> cancel{false};
  auto client = std::async(std::launch::async, [&] {
    CURL* easy = fake.request();
    const auto result = winhttp_perform(easy, [&] { return cancel.load(); });
    curl_easy_cleanup(easy); return result;
  });
  fake.opened.get_future().wait(); // connection opened; the fake peer never sends its HTTP answer
  cancel = true;
  CHECK(client.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  CHECK(client.get() == CURLE_ABORTED_BY_CALLBACK);
}
int main() {
  CHECK(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
  retained_connect_only(); cancel_slow_response();
  curl_global_cleanup();
  std::puts("yougame_http_cancel: 2 socketpair fake-I/O scenarios passed");
}
