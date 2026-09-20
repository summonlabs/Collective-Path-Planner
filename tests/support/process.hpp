// Collective Path Planner - real OS process control for multiprocess proofs.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// These helpers start a genuine second process (the cpathd coordinator) and can
// terminate it abruptly, so that crash/restart behaviour is proven against the
// operating system rather than simulated in-process.
#ifndef CPATH_TEST_PROCESS_HPP
#define CPATH_TEST_PROCESS_HPP

#include <string>
#include <vector>

namespace cpath_test {

class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  bool spawn(const std::string& executable, const std::vector<std::string>& arguments, std::string& error);
  bool running() const;
  // Terminates the process immediately and reaps it. This is a crash, not a
  // graceful shutdown, which is exactly what the restart proofs need.
  void kill();
  // Blocks until the process exits and returns its exit code.
  int join();
  bool exited() const { return exited_; }
  int exit_code() const { return exit_code_; }

 private:
  void* handle_{nullptr};
  unsigned long pid_{0};
  bool exited_{false};
  int exit_code_{-1};
};

// Process-startup synchronisation: waits until a file exists and is non-empty.
// This is not a test timeout; it exists because the coordinator picks an
// ephemeral port and publishes it there. Returns false when the budget is
// exhausted, which is reported as a failed proof rather than a hang.
bool wait_for_file(const std::string& path, unsigned budget_millis, std::string& content);

// Sleeps for the given number of milliseconds.
void sleep_millis(unsigned millis);

std::string read_text(const std::string& path);
bool write_text(const std::string& path, const std::string& content);
bool remove_file(const std::string& path);
bool make_directory(const std::string& path);
bool remove_directory_tree(const std::string& path);
std::string temporary_directory(const std::string& tag);

}  // namespace cpath_test

#endif  // CPATH_TEST_PROCESS_HPP
