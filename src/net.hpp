// Collective Path Planner - internal portable blocking socket layer.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// This header is private to the library: it is not installed and not part of
// the public API. It exists so that the coordinator and the client share one
// audited implementation of the small amount of socket code the library needs.
#ifndef CPATH_SRC_NET_HPP
#define CPATH_SRC_NET_HPP

#include <cstdint>
#include <span>
#include <string>

#include "cpath/status.hpp"

namespace cpath {
namespace internal {

// Process-wide socket subsystem. On Windows this performs WSAStartup exactly
// once. There is deliberately no matching teardown call: WSACleanup is optional,
// and calling it while a socket may still be alive is a well-known source of
// teardown-order defects.
Status net_initialize() noexcept;

#ifdef _WIN32
using NativeSocket = std::uintptr_t;
inline constexpr NativeSocket kInvalidSocket = static_cast<NativeSocket>(~static_cast<NativeSocket>(0));
#else
using NativeSocket = int;
inline constexpr NativeSocket kInvalidSocket = -1;
#endif

// RAII wrapper around one native socket handle. Move-only.
class Socket {
 public:
  Socket() = default;
  ~Socket();
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  bool is_open() const noexcept { return handle_ != kInvalidSocket; }
  NativeSocket native() const noexcept { return handle_; }
  // Closes the handle if open and resets the wrapper. Idempotent, never throws.
  void close() noexcept;

  static Result<Socket> listen_on(const std::string& address, std::uint16_t port, int backlog);
  static Result<Socket> connect_to(const std::string& address, std::uint16_t port);

  Result<Socket> accept() const;
  std::uint16_t local_port() const noexcept;
  Status set_nodelay() const noexcept;
  Status shutdown_both() const noexcept;

 private:
  explicit Socket(NativeSocket handle) : handle_(handle) {}
  NativeSocket handle_{kInvalidSocket};
};

// Writes every byte or fails; a partial write is completed, not reported.
Status send_all(const Socket& socket, std::span<const std::uint8_t> bytes);
// Reads at least one byte. A return value of 0 means the peer closed in order.
Result<std::size_t> receive_some(const Socket& socket, std::span<std::uint8_t> buffer);

enum class WaitOutcome : int {
  kError = 0,
  kFirst = 1,
  kSecond = 2,
};

// Blocks until the first socket (or, when not null, the second) becomes
// readable. There is no timeout: shutdown is driven by a WakeupPair instead, so
// that no protocol decision ever depends on an interval.
Result<WaitOutcome> wait_readable(const Socket& first, const Socket* second) noexcept;

// A connected loopback TCP pair used to interrupt a blocking wait. Where a
// self-pipe would be used on POSIX, a socket pair is used so the same select()
// based wait works on both platforms.
class WakeupPair {
 public:
  Status open();
  void close() noexcept;
  bool is_open() const noexcept { return reader_.is_open() && writer_.is_open(); }
  const Socket& reader() const noexcept { return reader_; }
  // Writes one byte; safe to call from any thread and never blocks for long.
  Status signal() noexcept;
  // Consumes any pending bytes so the pair can be reused.
  Status drain() noexcept;

 private:
  Socket reader_{};
  Socket writer_{};
};

}  // namespace internal
}  // namespace cpath

#endif  // CPATH_SRC_NET_HPP
