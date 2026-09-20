// Collective Path Planner - internal portable blocking socket layer.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "net.hpp"

#include <array>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cpath {
namespace internal {
namespace {

#ifdef _WIN32
constexpr int kWouldBlock = WSAEWOULDBLOCK;
constexpr int kInterrupted = WSAEINTR;
constexpr int kConnectionRefused = WSAECONNREFUSED;

int last_socket_error() noexcept { return ::WSAGetLastError(); }

void close_native(NativeSocket handle) noexcept { ::closesocket(handle); }

bool is_valid(NativeSocket handle) noexcept { return handle != INVALID_SOCKET; }

#else
constexpr int kWouldBlock = EWOULDBLOCK;
constexpr int kInterrupted = EINTR;
constexpr int kConnectionRefused = ECONNREFUSED;

int last_socket_error() noexcept { return errno; }

void close_native(NativeSocket handle) noexcept { ::close(handle); }

bool is_valid(NativeSocket handle) noexcept { return handle >= 0; }

#endif

std::once_flag g_init_once;
Status g_init_status = Status::ok();

void initialize_once() {
#ifdef _WIN32
  WSADATA data{};
  const int result = ::WSAStartup(MAKEWORD(2, 2), &data);
  if (result != 0) {
    g_init_status = Status::error(ErrorCode::kSocketFailure,
                                  "WSAStartup failed with code " + std::to_string(result));
    return;
  }
  g_init_status = Status::ok();
#else
  g_init_status = Status::ok();
#endif
}

Status resolve_ipv4(const std::string& address, std::uint16_t port, sockaddr_in& out) {
  std::memset(&out, 0, sizeof(out));
  out.sin_family = AF_INET;
  out.sin_port = htons(port);
  if (::inet_pton(AF_INET, address.c_str(), &out.sin_addr) == 1) {
    return Status::ok();
  }
  // Only literal IPv4 addresses and "localhost" are accepted: the transport is
  // loopback-scoped and must not depend on a resolver.
  if (address == "localhost") {
    out.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return Status::ok();
  }
  return Status::error(ErrorCode::kSocketFailure, "bind/connect address must be a literal IPv4 address: " + address);
}

}  // namespace

Status net_initialize() noexcept {
  std::call_once(g_init_once, initialize_once);
  return g_init_status;
}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) { other.handle_ = kInvalidSocket; }

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = kInvalidSocket;
  }
  return *this;
}

void Socket::close() noexcept {
  if (handle_ != kInvalidSocket) {
    close_native(handle_);
    handle_ = kInvalidSocket;
  }
}

Result<Socket> Socket::listen_on(const std::string& address, std::uint16_t port, int backlog) {
  if (Status status = net_initialize(); !status.is_ok()) {
    return Result<Socket>::failure(status);
  }
  sockaddr_in endpoint{};
  if (Status status = resolve_ipv4(address, port, endpoint); !status.is_ok()) {
    return Result<Socket>::failure(status);
  }
  const NativeSocket handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (!is_valid(handle)) {
    return Result<Socket>::failure(ErrorCode::kSocketFailure,
                                   "socket() failed with code " + std::to_string(last_socket_error()));
  }
  Socket socket(handle);
  const int reuse = 1;
  (void)::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
  if (::bind(handle, reinterpret_cast<const sockaddr*>(&endpoint), static_cast<int>(sizeof(endpoint))) != 0) {
    return Result<Socket>::failure(ErrorCode::kSocketFailure,
                                   "bind() failed with code " + std::to_string(last_socket_error()));
  }
  if (::listen(handle, backlog) != 0) {
    return Result<Socket>::failure(ErrorCode::kSocketFailure,
                                   "listen() failed with code " + std::to_string(last_socket_error()));
  }
  return Result<Socket>::success(std::move(socket));
}

Result<Socket> Socket::connect_to(const std::string& address, std::uint16_t port) {
  if (Status status = net_initialize(); !status.is_ok()) {
    return Result<Socket>::failure(status);
  }
  sockaddr_in endpoint{};
  if (Status status = resolve_ipv4(address, port, endpoint); !status.is_ok()) {
    return Result<Socket>::failure(status);
  }
  const NativeSocket handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (!is_valid(handle)) {
    return Result<Socket>::failure(ErrorCode::kSocketFailure,
                                   "socket() failed with code " + std::to_string(last_socket_error()));
  }
  Socket socket(handle);
  if (::connect(handle, reinterpret_cast<const sockaddr*>(&endpoint), static_cast<int>(sizeof(endpoint))) != 0) {
    const int error = last_socket_error();
    if (error == kConnectionRefused) {
      return Result<Socket>::failure(ErrorCode::kConnectionRefused,
                                     "no coordinator is listening on " + address + ":" + std::to_string(port));
    }
    return Result<Socket>::failure(ErrorCode::kSocketFailure,
                                   "connect() failed with code " + std::to_string(error));
  }
  (void)socket.set_nodelay();
  return Result<Socket>::success(std::move(socket));
}

Result<Socket> Socket::accept() const {
  if (!is_open()) {
    return Result<Socket>::failure(ErrorCode::kSocketFailure, "accept() on a closed socket");
  }
  sockaddr_in peer{};
#ifdef _WIN32
  int length = static_cast<int>(sizeof(peer));
#else
  socklen_t length = sizeof(peer);
#endif
  const NativeSocket handle = ::accept(handle_, reinterpret_cast<sockaddr*>(&peer), &length);
  if (!is_valid(handle)) {
    const int error = last_socket_error();
    if (error == kInterrupted || error == kWouldBlock) {
      return Result<Socket>::failure(ErrorCode::kCancelled, "accept() was interrupted");
    }
    return Result<Socket>::failure(ErrorCode::kSocketFailure,
                                   "accept() failed with code " + std::to_string(error));
  }
  Socket socket(handle);
  (void)socket.set_nodelay();
  return Result<Socket>::success(std::move(socket));
}

std::uint16_t Socket::local_port() const noexcept {
  if (!is_open()) {
    return 0;
  }
  sockaddr_in endpoint{};
#ifdef _WIN32
  int length = static_cast<int>(sizeof(endpoint));
#else
  socklen_t length = sizeof(endpoint);
#endif
  if (::getsockname(handle_, reinterpret_cast<sockaddr*>(&endpoint), &length) != 0) {
    return 0;
  }
  return ntohs(endpoint.sin_port);
}

Status Socket::set_nodelay() const noexcept {
  if (!is_open()) {
    return Status::error(ErrorCode::kSocketFailure, "set_nodelay() on a closed socket");
  }
  const int enabled = 1;
  if (::setsockopt(handle_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled),
                   sizeof(enabled)) != 0) {
    return Status::error(ErrorCode::kSocketFailure,
                         "TCP_NODELAY failed with code " + std::to_string(last_socket_error()));
  }
  return Status::ok();
}

Status Socket::shutdown_both() const noexcept {
  if (!is_open()) {
    return Status::ok();
  }
#ifdef _WIN32
  (void)::shutdown(handle_, SD_BOTH);
#else
  (void)::shutdown(handle_, SHUT_RDWR);
#endif
  return Status::ok();
}

Status send_all(const Socket& socket, std::span<const std::uint8_t> bytes) {
  if (!socket.is_open()) {
    return Status::error(ErrorCode::kSendFailure, "send on a closed socket");
  }
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const std::size_t remaining = bytes.size() - sent;
    const std::size_t chunk = remaining > 0x7FFFFFFFu ? 0x7FFFFFFFu : remaining;
#ifdef _WIN32
    const int written = ::send(socket.native(), reinterpret_cast<const char*>(bytes.data() + sent),
                               static_cast<int>(chunk), 0);
#else
    const int written = static_cast<int>(::send(socket.native(), bytes.data() + sent, chunk, MSG_NOSIGNAL));
#endif
    if (written <= 0) {
      const int error = last_socket_error();
      if (error == kInterrupted) {
        continue;
      }
      return Status::error(ErrorCode::kSendFailure, "send failed with code " + std::to_string(error));
    }
    sent += static_cast<std::size_t>(written);
  }
  return Status::ok();
}

Result<std::size_t> receive_some(const Socket& socket, std::span<std::uint8_t> buffer) {
  if (!socket.is_open()) {
    return Result<std::size_t>::failure(ErrorCode::kReceiveFailure, "receive on a closed socket");
  }
  if (buffer.empty()) {
    return Result<std::size_t>::failure(ErrorCode::kReceiveFailure, "receive into an empty buffer");
  }
  const std::size_t chunk = buffer.size() > 0x7FFFFFFFu ? 0x7FFFFFFFu : buffer.size();
  while (true) {
#ifdef _WIN32
    const int received = ::recv(socket.native(), reinterpret_cast<char*>(buffer.data()),
                                static_cast<int>(chunk), 0);
#else
    const int received = static_cast<int>(::recv(socket.native(), buffer.data(), chunk, 0));
#endif
    if (received > 0) {
      return Result<std::size_t>::success(static_cast<std::size_t>(received));
    }
    if (received == 0) {
      return Result<std::size_t>::success(0);  // orderly close
    }
    const int error = last_socket_error();
    if (error == kInterrupted) {
      continue;
    }
    return Result<std::size_t>::failure(ErrorCode::kReceiveFailure,
                                        "recv failed with code " + std::to_string(error));
  }
}

Result<WaitOutcome> wait_readable(const Socket& first, const Socket* second) noexcept {
  if (!first.is_open()) {
    return Result<WaitOutcome>::failure(ErrorCode::kSocketFailure, "wait on a closed socket");
  }
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(first.native(), &read_set);
  NativeSocket highest = first.native();
  if (second != nullptr && second->is_open()) {
    FD_SET(second->native(), &read_set);
    if (second->native() > highest) {
      highest = second->native();
    }
  }
  // Windows ignores the first argument of select(); POSIX needs the highest
  // descriptor plus one. This is the only platform difference in the wait.
#ifdef _WIN32
  (void)highest;
  const int result = ::select(0, &read_set, nullptr, nullptr, nullptr);
#else
  const int result = ::select(static_cast<int>(highest) + 1, &read_set, nullptr, nullptr, nullptr);
#endif
  if (result < 0) {
    const int error = last_socket_error();
    if (error == kInterrupted) {
      return Result<WaitOutcome>::success(WaitOutcome::kSecond);
    }
    return Result<WaitOutcome>::failure(ErrorCode::kSocketFailure,
                                        "select failed with code " + std::to_string(error));
  }
  if (second != nullptr && second->is_open() && FD_ISSET(second->native(), &read_set)) {
    return Result<WaitOutcome>::success(WaitOutcome::kSecond);
  }
  if (FD_ISSET(first.native(), &read_set)) {
    return Result<WaitOutcome>::success(WaitOutcome::kFirst);
  }
  return Result<WaitOutcome>::failure(ErrorCode::kSocketFailure, "select reported no readable socket");
}

Status WakeupPair::open() {
  auto listener = Socket::listen_on("127.0.0.1", 0, 1);
  if (!listener.has_value()) {
    return listener.status();
  }
  const std::uint16_t port = listener.value().local_port();
  if (port == 0) {
    return Status::error(ErrorCode::kSocketFailure, "wakeup listener did not report a port");
  }
  auto client = Socket::connect_to("127.0.0.1", port);
  if (!client.has_value()) {
    return client.status();
  }
  auto server = listener.value().accept();
  if (!server.has_value()) {
    return server.status();
  }
  listener.value().close();
  reader_ = std::move(server.value());
  writer_ = std::move(client.value());
  return Status::ok();
}

void WakeupPair::close() noexcept {
  reader_.close();
  writer_.close();
}

Status WakeupPair::signal() noexcept {
  if (!writer_.is_open()) {
    return Status::error(ErrorCode::kSocketFailure, "wakeup pair is not open");
  }
  const std::array<std::uint8_t, 1> byte{1u};
  return send_all(writer_, byte);
}

Status WakeupPair::drain() noexcept {
  if (!reader_.is_open()) {
    return Status::error(ErrorCode::kSocketFailure, "wakeup pair is not open");
  }
  std::array<std::uint8_t, 64> buffer{};
  while (true) {
    auto received = receive_some(reader_, buffer);
    if (!received.has_value()) {
      return received.status();
    }
    if (received.value() == 0) {
      return Status::ok();  // peer closed: nothing more to drain
    }
    if (received.value() < buffer.size()) {
      return Status::ok();
    }
  }
}

}  // namespace internal
}  // namespace cpath
