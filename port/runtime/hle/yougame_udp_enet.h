// ENet's intercept runs after receive and before its first header read (protocol.c).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yougame_udp.h"
#include <enet/enet.h>

namespace yougame::udp {
struct EnetIngress {
  explicit EnetIngress(DropCounter* counter = nullptr) : senders(counter) {}
  Ingress senders;
  uint64_t (*clock)();
  void (*log)(const char*);
  static int ENET_CALLBACK intercept(ENetHost* host, ENetEvent* event) {
    auto& self = *static_cast<EnetIngress*>(host->interceptContext);
    // ENet uses IPv4 sockets. wakeup_thread sends this byte from host->socket
    // itself to 127.0.0.1, so the source must also have our actual bound port.
    const uint8_t loopback[4] = {127, 0, 0, 1};
    if (host->address.port && host->receivedAddress.port == host->address.port &&
        std::memcmp(&host->receivedAddress.host, loopback, sizeof loopback) == 0 &&
        host->receivedDataLength == 1 && host->receivedData[0] == 0) {
      if (event) event->type = (ENetEventType)42; // end service wait, as in Slippi's wake intercept
      return 1; // never a peer, never counted, never parsed
    }
    return self.senders.accept({host->receivedAddress.host, host->receivedAddress.port}, self.clock(), self.log) ? 0 : 1;
  }
  void attach(ENetHost* host) {
    host->interceptContext = this;
    host->intercept = intercept;
  }
};
} // namespace yougame::udp
