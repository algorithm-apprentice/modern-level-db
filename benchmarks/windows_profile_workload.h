#ifndef MODERN_LEVELDB_BENCHMARKS_WINDOWS_PROFILE_WORKLOAD_H_
#define MODERN_LEVELDB_BENCHMARKS_WINDOWS_PROFILE_WORKLOAD_H_

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "windows_profile_protocol.h"

namespace modern_leveldb::profiling::windows {

class Workload final {
 public:
  Workload() = default;
  Workload(const Workload&) = delete;
  Workload& operator=(const Workload&) = delete;
  ~Workload() {
    if (control_ != nullptr) {
      if (!::UnmapViewOfFile(control_)) {
        std::fprintf(stderr, "native workload cleanup unmap failed (Win32 %lu)\n",
                     ::GetLastError());
      }
    }
    for (HANDLE handle : {mapping_, ready_, proceed_}) {
      if (handle != nullptr) {
        if (!::CloseHandle(handle)) {
          std::fprintf(stderr, "native workload cleanup close failed (Win32 %lu)\n",
                       ::GetLastError());
        }
      }
    }
  }

  void Initialize(std::string_view mapping, std::string_view ready, std::string_view proceed,
                  std::filesystem::path ledger) {
    mapping_ = ParseHandle(mapping);
    ready_ = ParseHandle(ready);
    proceed_ = ParseHandle(proceed);
    ledger_ = std::move(ledger);
    if (ledger_.empty() || std::filesystem::exists(ledger_)) {
      throw std::runtime_error("native epoch ledger must be a new file");
    }
    control_ =
        static_cast<Control*>(::MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, ControlBytes));
    if (control_ == nullptr) {
      Fail("MapViewOfFile(profile control)", ::GetLastError());
    }
    if (control_->magic != ControlMagic || control_->version != ControlVersion ||
        control_->pid != ::GetCurrentProcessId()) {
      throw std::runtime_error("invalid owned Windows profiling control");
    }
  }

  void Begin(std::int64_t iterations) {
    if (control_ == nullptr || iterations <= 0) {
      throw std::runtime_error("native profile interval has no valid control/iterations");
    }
    if (!started_) {
      if (!::SetEvent(ready_)) {
        Fail("SetEvent(profile readiness)", ::GetLastError());
      }
      const DWORD waited = ::WaitForSingleObject(proceed_, 30'000);
      if (waited != WAIT_OBJECT_0) {
        Fail("WaitForSingleObject(profile proceed)",
             waited == WAIT_FAILED ? ::GetLastError() : waited);
      }
      started_ = true;
    }
    Epoch epoch{};
    epoch.id = static_cast<std::uint64_t>(::InterlockedIncrement64(&control_->epoch));
    epoch.expected = static_cast<std::uint64_t>(iterations);
    epoch.cpu_start = ProcessCpu();
    epoch.start = Counter();
    epochs_.push_back(epoch);
    ::InterlockedExchange(&control_->active, 1);
  }

  void End(std::int64_t iterations) noexcept {
    ::InterlockedExchange(&control_->active, 0);
    if (epochs_.empty() || iterations < 0) {
      ::InterlockedExchange(&control_->failure, ERROR_INVALID_DATA);
      return;
    }
    auto& epoch = epochs_.back();
    epoch.completed = static_cast<std::uint64_t>(iterations);
    LARGE_INTEGER counter{};
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!::QueryPerformanceCounter(&counter) ||
        !::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user)) {
      const DWORD error = ::GetLastError();
      ::InterlockedExchange(&control_->failure,
                            static_cast<LONG>(error != 0 ? error : ERROR_GEN_FAILURE));
      return;
    }
    epoch.end = static_cast<std::uint64_t>(counter.QuadPart);
    epoch.cpu_end = FileTime(kernel) + FileTime(user);
  }

  void Finish() {
    if (control_ == nullptr || epochs_.empty() || Observe(*control_).failure != 0) {
      throw std::runtime_error("native profiling epoch protocol failed");
    }
    LARGE_INTEGER frequency{};
    if (!::QueryPerformanceFrequency(&frequency)) {
      Fail("QueryPerformanceFrequency", ::GetLastError());
    }
    const std::filesystem::path temporary = ledger_.native() + L".tmp";
    std::ofstream out(temporary, std::ios::binary);
    if (!out) {
      throw std::runtime_error("could not create native epoch ledger");
    }
    out << "{\"schema_version\":1,\"pid\":" << ::GetCurrentProcessId()
        << ",\"qpc_frequency\":" << frequency.QuadPart << ",\"epochs\":[";
    for (std::size_t index = 0; index < epochs_.size(); ++index) {
      const auto& e = epochs_[index];
      if (e.expected != e.completed || e.end < e.start || e.cpu_end < e.cpu_start) {
        throw std::runtime_error("incomplete native measured epoch");
      }
      if (index != 0) out << ',';
      out << "{\"id\":" << e.id << ",\"expected_iterations\":" << e.expected
          << ",\"completed_iterations\":" << e.completed << ",\"start_qpc\":" << e.start
          << ",\"end_qpc\":" << e.end << ",\"cpu_100ns\":" << e.cpu_end - e.cpu_start << '}';
    }
    out << "]}\n";
    out.close();
    if (!out) {
      throw std::runtime_error("native epoch ledger write failed");
    }
    std::filesystem::rename(temporary, ledger_);
    if (!::UnmapViewOfFile(control_)) {
      Fail("UnmapViewOfFile(profile control)", ::GetLastError());
    }
    control_ = nullptr;
    for (HANDLE* handle : {&mapping_, &ready_, &proceed_}) {
      if (!::CloseHandle(*handle)) {
        Fail("CloseHandle(profile control)", ::GetLastError());
      }
      *handle = nullptr;
    }
  }

 private:
  struct Epoch {
    std::uint64_t id = 0, expected = 0, completed = 0, start = 0, end = 0;
    std::uint64_t cpu_start = 0, cpu_end = 0;
  };
  static HANDLE ParseHandle(std::string_view text) {
    std::uintptr_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0 ||
        value == std::numeric_limits<std::uintptr_t>::max()) {
      throw std::runtime_error("invalid native profile handle");
    }
    return reinterpret_cast<HANDLE>(value);
  }
  [[noreturn]] static void Fail(const char* operation, DWORD code) {
    throw std::runtime_error(std::string(operation) + " failed (Win32 " + std::to_string(code) +
                             ")");
  }
  static std::uint64_t Counter() {
    LARGE_INTEGER counter{};
    if (!::QueryPerformanceCounter(&counter)) Fail("QueryPerformanceCounter", ::GetLastError());
    return static_cast<std::uint64_t>(counter.QuadPart);
  }
  static std::uint64_t ProcessCpu() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user)) {
      Fail("GetProcessTimes", ::GetLastError());
    }
    return FileTime(kernel) + FileTime(user);
  }

  HANDLE mapping_ = nullptr, ready_ = nullptr, proceed_ = nullptr;
  Control* control_ = nullptr;
  std::filesystem::path ledger_;
  std::vector<Epoch> epochs_;
  bool started_ = false;
};

}  // namespace modern_leveldb::profiling::windows

#endif  // MODERN_LEVELDB_BENCHMARKS_WINDOWS_PROFILE_WORKLOAD_H_
