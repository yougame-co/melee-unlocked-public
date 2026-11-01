// Keep synchronous compatibility calls cancellable without closing live libcurl objects.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <curl/curl.h>
#include <functional>
inline CURLcode winhttp_perform(CURL* easy, const std::function<bool()>& cancelled, CURLM** keep = nullptr) {
  if (!cancelled) return curl_easy_perform(easy);
  CURLM* multi = curl_multi_init();
  if (!multi) return CURLE_FAILED_INIT;
  curl_multi_add_handle(multi, easy);
  CURLcode result = CURLE_ABORTED_BY_CALLBACK;
  int running = 1;
  while (!cancelled()) {
    if (curl_multi_perform(multi, &running) != CURLM_OK) break;
    if (!running) {
      int remaining = 0;
      if (auto* message = curl_multi_info_read(multi, &remaining)) result = message->data.result;
      break;
    }
    int count = 0;
    if (curl_multi_poll(multi, nullptr, 0, 50, &count) != CURLM_OK) break;
  }
  // CONNECT_ONLY sockets must remain attached to their multi handle while send/recv uses them.
  if (keep && result == CURLE_OK) *keep = multi;
  else {
    curl_multi_remove_handle(multi, easy);
    curl_multi_cleanup(multi);
  }
  return result;
}
