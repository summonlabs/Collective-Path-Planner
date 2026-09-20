// Collective Path Planner - real OS process control for multiprocess proofs.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "process.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace cpath_test {

void sleep_millis(unsigned millis) { std::this_thread::sleep_for(std::chrono::milliseconds(millis)); }

ChildProcess::~ChildProcess() {
  if (running()) {
    kill();
  }
#ifdef _WIN32
  if (handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#endif
}

#ifdef _WIN32
namespace {

std::string quote_argument(const std::string& argument) {
  std::string out = "\"";
  for (const char ch : argument) {
    if (ch == '"') {
      out += "\\\"";
    } else {
      out.push_back(ch);
    }
  }
  out += "\"";
  return out;
}

}  // namespace

bool ChildProcess::spawn(const std::string& executable, const std::vector<std::string>& arguments,
                         std::string& error) {
  std::string command_line = quote_argument(executable);
  for (const std::string& argument : arguments) {
    command_line += " ";
    command_line += quote_argument(argument);
  }
  std::vector<char> mutable_line(command_line.begin(), command_line.end());
  mutable_line.push_back('\0');

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION information{};
  const BOOL created = ::CreateProcessA(nullptr, mutable_line.data(), nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information);
  if (created == FALSE) {
    error = "CreateProcess failed with code " + std::to_string(::GetLastError());
    return false;
  }
  ::CloseHandle(information.hThread);
  handle_ = information.hProcess;
  pid_ = information.dwProcessId;
  exited_ = false;
  exit_code_ = -1;
  return true;
}

bool ChildProcess::running() const {
  if (handle_ == nullptr || exited_) {
    return false;
  }
  const DWORD state = ::WaitForSingleObject(static_cast<HANDLE>(handle_), 0);
  return state == WAIT_TIMEOUT;
}

void ChildProcess::kill() {
  if (handle_ == nullptr || exited_) {
    return;
  }
  ::TerminateProcess(static_cast<HANDLE>(handle_), 0xDEADu);
  ::WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
  DWORD code = 0;
  ::GetExitCodeProcess(static_cast<HANDLE>(handle_), &code);
  exit_code_ = static_cast<int>(code);
  exited_ = true;
}

int ChildProcess::join() {
  if (handle_ == nullptr) {
    return -1;
  }
  if (!exited_) {
    ::WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(static_cast<HANDLE>(handle_), &code);
    exit_code_ = static_cast<int>(code);
    exited_ = true;
  }
  return exit_code_;
}
#else
bool ChildProcess::spawn(const std::string& executable, const std::vector<std::string>& arguments,
                         std::string& error) {
  const pid_t child = ::fork();
  if (child < 0) {
    error = "fork failed";
    return false;
  }
  if (child == 0) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(executable.c_str(), argv.data());
    ::_exit(127);
  }
  pid_ = static_cast<unsigned long>(child);
  handle_ = reinterpret_cast<void*>(static_cast<std::uintptr_t>(child));
  exited_ = false;
  exit_code_ = -1;
  return true;
}

bool ChildProcess::running() const {
  if (handle_ == nullptr || exited_) {
    return false;
  }
  int status = 0;
  const pid_t result = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
  return result == 0;
}

void ChildProcess::kill() {
  if (handle_ == nullptr || exited_) {
    return;
  }
  ::kill(static_cast<pid_t>(pid_), SIGKILL);
  int status = 0;
  ::waitpid(static_cast<pid_t>(pid_), &status, 0);
  exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  exited_ = true;
}

int ChildProcess::join() {
  if (handle_ == nullptr) {
    return -1;
  }
  if (!exited_) {
    int status = 0;
    ::waitpid(static_cast<pid_t>(pid_), &status, 0);
    exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    exited_ = true;
  }
  return exit_code_;
}
#endif

bool wait_for_file(const std::string& path, unsigned budget_millis, std::string& content) {
  const unsigned step = 10;
  for (unsigned elapsed = 0; elapsed <= budget_millis; elapsed += step) {
    std::ifstream stream(path, std::ios::binary);
    if (stream) {
      std::ostringstream buffer;
      buffer << stream.rdbuf();
      if (!buffer.str().empty()) {
        content = buffer.str();
        while (!content.empty() && (content.back() == '\n' || content.back() == '\r')) {
          content.pop_back();
        }
        return true;
      }
    }
    sleep_millis(step);
  }
  return false;
}

std::string read_text(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return std::string();
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

bool write_text(const std::string& path, const std::string& content) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return false;
  }
  stream << content;
  stream.flush();
  return static_cast<bool>(stream);
}

bool remove_file(const std::string& path) { return std::remove(path.c_str()) == 0; }

bool make_directory(const std::string& path) {
#ifdef _WIN32
  return ::CreateDirectoryA(path.c_str(), nullptr) != FALSE || ::GetLastError() == ERROR_ALREADY_EXISTS;
#else
  return ::mkdir(path.c_str(), 0700) == 0 || errno == EEXIST;
#endif
}

bool remove_directory_tree(const std::string& path) {
#ifdef _WIN32
  std::string command = "cmd /c rmdir /s /q \"" + path + "\"";
#else
  std::string command = "rm -rf \"" + path + "\"";
#endif
  return std::system(command.c_str()) == 0;
}

std::string temporary_directory(const std::string& tag) {
  static unsigned counter = 0;
  ++counter;
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
  char* base = nullptr;
  std::size_t base_size = 0;
  const bool have_base = ::_dupenv_s(&base, &base_size, "TEMP") == 0 && base != nullptr;
  std::string root = have_base ? std::string(base) : std::string(".");
  std::free(base);
  const char separator = '\\';
#else
  const char* base = std::getenv("TMPDIR");
  std::string root = base != nullptr ? base : "/tmp";
  const char separator = '/';
#endif
  const std::string path = root + separator + "cpath-" + tag + "-" + std::to_string(now) + "-" +
                           std::to_string(counter);
  make_directory(path);
  return path;
}

}  // namespace cpath_test
