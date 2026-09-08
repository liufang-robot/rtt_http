// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <httplib.h>
#include <stdexcept>

// Actual wire requests avoid a client library's URL normalization and permit
// incomplete HTTP requests/TLS handshakes during shutdown tests.
class RawClient {
public:
  explicit RawClient(int port) : socket_(::socket(AF_INET, SOCK_STREAM, 0)) {
    if (socket_ == INVALID_SOCKET) {
      throw std::runtime_error("create test socket");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::connect(socket_, reinterpret_cast<sockaddr *>(&address),
                  sizeof(address))) {
      httplib::detail::close_socket(socket_);
      throw std::runtime_error("connect test socket");
    }
    httplib::detail::set_socket_opt_time(socket_, SOL_SOCKET, SO_RCVTIMEO, 2,
                                         0);
  }
  ~RawClient() {
    httplib::detail::shutdown_socket(socket_);
    httplib::detail::close_socket(socket_);
  }
  RawClient(const RawClient &) = delete;
  RawClient &operator=(const RawClient &) = delete;
  void send(const std::string &text) {
    std::size_t offset = 0;
    while (offset != text.size()) {
      const auto size = httplib::detail::send_socket(
          socket_, text.data() + offset, text.size() - offset,
          CPPHTTPLIB_SEND_FLAGS);
      if (size <= 0) {
        throw std::runtime_error("send test request");
      }
      offset += static_cast<std::size_t>(size);
    }
  }
  bool disconnected(time_t timeout_seconds = 2) {
    char byte;
    return httplib::detail::select_read(socket_, timeout_seconds, 0) > 0 &&
           httplib::detail::read_socket(socket_, &byte, 1, 0) <= 0;
  }
  std::string receive() {
    std::string result;
    char bytes[4096];
    for (;;) {
      const auto count =
          httplib::detail::read_socket(socket_, bytes, sizeof(bytes), 0);
      if (count <= 0) {
        return result;
      }
      result.append(bytes, static_cast<std::size_t>(count));
      if (result.size() > 65536) {
        throw std::runtime_error("test wire response exceeds limit");
      }
    }
  }

private:
  socket_t socket_;
};
