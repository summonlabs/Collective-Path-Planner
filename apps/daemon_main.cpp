// cpathd - Collective Path Planner coordinator process.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "cpath/server.hpp"
#include "cpath/version.hpp"

namespace {

std::atomic<bool> g_stop_requested{false};
std::mutex g_stop_mutex;
std::condition_variable g_stop_cv;

extern "C" void handle_signal(int) {
  g_stop_requested.store(true);
  g_stop_cv.notify_all();
}

struct Options {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{0};
  std::string store_root{};
  std::string label{"cpathd"};
  std::size_t max_sessions{cpath::kMaxSessions};
  std::string port_file{};
  bool quiet{false};
};

void print_usage() {
  std::cout << "cpathd " << cpath::version_string() << " - Collective Path Planner coordinator\n"
            << "\n"
            << "usage: cpathd [options]\n"
            << "\n"
            << "  --bind ADDRESS     address to listen on (default 127.0.0.1)\n"
            << "  --port PORT        TCP port, 0 selects an ephemeral port (default 0)\n"
            << "  --store DIRECTORY  open a durable plan store in DIRECTORY\n"
            << "  --label NAME       service label reported to clients (default cpathd)\n"
            << "  --max-sessions N   concurrent session bound (default " << cpath::kMaxSessions << ")\n"
            << "  --port-file PATH   write the bound port to PATH once listening\n"
            << "  --quiet            suppress the startup banner\n"
            << "  --version          print the build identification and exit\n"
            << "  --help             print this message and exit\n";
}

bool parse_size(std::string_view text, std::size_t& out) {
  if (text.empty()) {
    return false;
  }
  std::size_t value = 0;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    value = value * 10u + static_cast<std::size_t>(ch - '0');
    if (value > 1000000u) {
      return false;
    }
  }
  out = value;
  return true;
}

bool parse_port(std::string_view text, std::uint16_t& out) {
  std::size_t value = 0;
  if (!parse_size(text, value) || value > 65535u) {
    return false;
  }
  out = static_cast<std::uint16_t>(value);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  std::vector<std::string> args(argv + 1, argv + argc);
  for (std::size_t index = 0; index < args.size(); ++index) {
    const std::string& arg = args[index];
    const auto next = [&](const char* name) -> const std::string* {
      if (index + 1u >= args.size()) {
        std::cerr << "cpathd: " << name << " requires a value\n";
        return nullptr;
      }
      ++index;
      return &args[index];
    };
    if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    }
    if (arg == "--version") {
      std::cout << cpath::build_summary() << "\n";
      return 0;
    }
    if (arg == "--quiet") {
      options.quiet = true;
      continue;
    }
    if (arg == "--bind") {
      const std::string* value = next("--bind");
      if (value == nullptr) {
        return 1;
      }
      options.bind_address = *value;
      continue;
    }
    if (arg == "--label") {
      const std::string* value = next("--label");
      if (value == nullptr) {
        return 1;
      }
      options.label = *value;
      continue;
    }
    if (arg == "--store") {
      const std::string* value = next("--store");
      if (value == nullptr) {
        return 1;
      }
      options.store_root = *value;
      continue;
    }
    if (arg == "--port-file") {
      const std::string* value = next("--port-file");
      if (value == nullptr) {
        return 1;
      }
      options.port_file = *value;
      continue;
    }
    if (arg == "--port") {
      const std::string* value = next("--port");
      if (value == nullptr || !parse_port(*value, options.port)) {
        std::cerr << "cpathd: --port requires a number between 0 and 65535\n";
        return 1;
      }
      continue;
    }
    if (arg == "--max-sessions") {
      const std::string* value = next("--max-sessions");
      if (value == nullptr || !parse_size(*value, options.max_sessions) || options.max_sessions == 0) {
        std::cerr << "cpathd: --max-sessions requires a positive number\n";
        return 1;
      }
      continue;
    }
    std::cerr << "cpathd: unknown argument " << arg << "\n";
    print_usage();
    return 1;
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  cpath::ServerConfig config;
  config.bind_address = options.bind_address;
  config.port = options.port;
  config.store_root = options.store_root;
  config.label = options.label;
  config.max_sessions = options.max_sessions;

  cpath::Coordinator coordinator(config);
  const cpath::Status started = coordinator.start();
  if (!started.is_ok()) {
    std::cerr << "cpathd: failed to start: " << started.to_string() << "\n";
    return 2;
  }

  if (!options.quiet) {
    std::cout << cpath::build_summary() << "\n";
    std::cout << "cpathd listening on " << coordinator.bind_address() << ":" << coordinator.port()
              << " epoch=" << coordinator.epoch().value()
              << " incarnation=" << coordinator.incarnation().value();
    if (!options.store_root.empty()) {
      std::cout << " store=" << options.store_root;
    }
    std::cout << "\n";
  }
  std::cout.flush();

  if (!options.port_file.empty()) {
    std::ofstream port_file(options.port_file, std::ios::binary | std::ios::trunc);
    if (!port_file) {
      std::cerr << "cpathd: cannot write port file " << options.port_file << "\n";
      coordinator.stop();
      return 2;
    }
    port_file << coordinator.port() << "\n";
    port_file.flush();
    if (!port_file) {
      std::cerr << "cpathd: failed to flush port file " << options.port_file << "\n";
      coordinator.stop();
      return 2;
    }
  }

  // Wait until either an operator signal arrives or the coordinator stops itself
  // because a client sent a valid shutdown request. The condition variable is
  // notified by the signal handler; the periodic wake-up exists so that a signal
  // delivered while the handler is unavailable, and a coordinator that retired
  // itself, both unblock the process. No protocol decision depends on it.
  while (!g_stop_requested.load() && coordinator.running()) {
    std::unique_lock<std::mutex> lock(g_stop_mutex);
    g_stop_cv.wait_for(lock, std::chrono::seconds(1), [] { return g_stop_requested.load(); });
  }

  const cpath::Status stopped = coordinator.stop();
  if (!stopped.is_ok()) {
    std::cerr << "cpathd: shutdown reported: " << stopped.to_string() << "\n";
    return 3;
  }
  const cpath::ServerStats stats = coordinator.stats();
  if (!options.quiet) {
    std::cout << "cpathd stopped: sessions=" << stats.sessions_accepted
              << " rejected=" << stats.sessions_rejected << " frames-in=" << stats.frames_received
              << " frames-out=" << stats.frames_sent << " frames-rejected=" << stats.frames_rejected
              << " plans=" << stats.plans_produced << " denials=" << stats.plan_denials
              << " replays-refused=" << stats.replays_refused << " errors=" << stats.errors << "\n";
  }
  return 0;
}
