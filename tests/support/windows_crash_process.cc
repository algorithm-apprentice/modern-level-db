#include "support/windows_crash_process.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

#include "support/crash_observation.h"

namespace modern_leveldb::test_support {
namespace {

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
using Milliseconds = std::chrono::milliseconds;

constexpr Milliseconds kReadinessLimit{30'000};
constexpr Milliseconds kCompletionLimit{10'000};
constexpr Milliseconds kCleanupLimit{5'000};
constexpr DWORD kForcedExitCode = 1;
constexpr std::size_t kObservationLimit = 4'096;

Error NativeError(const char* operation, DWORD code) {
  return Error::Io(std::string(operation) + " failed (Win32 " + std::to_string(code) + ")");
}

Error TimeoutError(const char* operation) {
  return Error::Aborted(std::string(operation) + " timed out (Win32 " +
                        std::to_string(WAIT_TIMEOUT) + ")");
}

void ReportCleanupFailure(const char* operation, DWORD code) noexcept {
  std::fprintf(stderr, "Windows crash process cleanup: %s failed (Win32 %lu)\n", operation,
               static_cast<unsigned long>(code));
}

DWORD RemainingMilliseconds(Deadline deadline, DWORD maximum) noexcept {
  const auto now = Clock::now();
  if (now >= deadline) {
    return 0;
  }
  const auto remaining = std::chrono::ceil<Milliseconds>(deadline - now).count();
  return static_cast<DWORD>(
      std::min(remaining, static_cast<Milliseconds::rep>(maximum)));
}

class OwnedHandle final {
 public:
  explicit OwnedHandle(const char* close_operation) noexcept : close_operation_(close_operation) {}
  OwnedHandle(const OwnedHandle&) = delete;
  OwnedHandle& operator=(const OwnedHandle&) = delete;
  ~OwnedHandle() { CloseAndReport(); }

  void Adopt(HANDLE handle) noexcept { handle_ = handle; }
  [[nodiscard]] HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

  [[nodiscard]] Status Close() {
    if (handle_ != nullptr) {
      if (!::CloseHandle(handle_)) {
        return std::unexpected(NativeError(close_operation_, ::GetLastError()));
      }
      handle_ = nullptr;
    }
    return {};
  }

  void CloseAndReport() noexcept {
    if (handle_ != nullptr) {
      if (!::CloseHandle(handle_)) {
        ReportCleanupFailure(close_operation_, ::GetLastError());
        return;
      }
      handle_ = nullptr;
    }
  }

 private:
  HANDLE handle_ = nullptr;
  const char* close_operation_;
};

class LaunchAttributes final {
 public:
  LaunchAttributes() = default;
  LaunchAttributes(const LaunchAttributes&) = delete;
  LaunchAttributes& operator=(const LaunchAttributes&) = delete;
  ~LaunchAttributes() {
    if (initialized_) {
      ::DeleteProcThreadAttributeList(get());
    }
  }

  [[nodiscard]] Status Initialize() {
    SIZE_T bytes = 0;
    const BOOL queried = ::InitializeProcThreadAttributeList(nullptr, 2, 0, &bytes);
    const DWORD query_error = ::GetLastError();
    if (queried) {
      return std::unexpected(
          NativeError("InitializeProcThreadAttributeList(size)", ERROR_INVALID_DATA));
    }
    if (query_error != ERROR_INSUFFICIENT_BUFFER) {
      return std::unexpected(NativeError("InitializeProcThreadAttributeList(size)", query_error));
    }
    if (bytes == 0) {
      return std::unexpected(
          NativeError("InitializeProcThreadAttributeList(size)", ERROR_INVALID_DATA));
    }
    storage_ = std::make_unique<std::byte[]>(bytes);
    if (!::InitializeProcThreadAttributeList(get(), 2, 0, &bytes)) {
      return std::unexpected(NativeError("InitializeProcThreadAttributeList", ::GetLastError()));
    }
    initialized_ = true;
    return {};
  }

  [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept {
    return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.get());
  }

 private:
  std::unique_ptr<std::byte[]> storage_;
  bool initialized_ = false;
};

std::wstring QuoteArgument(std::wstring_view argument) {
  std::wstring quoted(1, L'"');
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    quoted.append(character == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
    quoted.push_back(character);
    backslashes = 0;
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

bool IsFixedMode(std::wstring_view mode) noexcept {
  return mode == L"exit" || mode == L"hold" || mode == L"malformed" || mode == L"tail" ||
         mode == L"stall";
}

bool HasEmbeddedNull(std::wstring_view argument) noexcept {
  return argument.find(L'\0') != std::wstring_view::npos;
}

enum class PipeState { Idle, Data, Eof };

}  // namespace

struct WindowsCrashProcess::Impl final {
  OwnedHandle read{"CloseHandle(observation read)"};
  OwnedHandle write{"CloseHandle(observation write)"};
  OwnedHandle process{"CloseHandle(child process)"};
  OwnedHandle thread{"CloseHandle(child thread)"};
  OwnedHandle job{"CloseHandle(child job)"};
  CrashObservation observation;
  std::optional<Error> first_error;
  std::optional<std::uint32_t> exit_code;
  std::size_t observed_bytes = 0;
  bool terminal = false;
  bool pipe_eof = false;
  bool completion_observed = false;
  bool termination_requested = false;

  ~Impl() { Cleanup(); }

  void Remember(Error error) {
    if (!first_error.has_value()) {
      first_error = std::move(error);
    }
  }

  [[nodiscard]] Status CurrentStatus() const {
    if (first_error.has_value()) {
      return std::unexpected(*first_error);
    }
    return {};
  }

  [[nodiscard]] Status Fail(Error error) {
    Remember(std::move(error));
    return CurrentStatus();
  }

  [[nodiscard]] Result<bool> ObserveTerminal() {
    if (terminal) {
      return true;
    }
    const DWORD waited = ::WaitForSingleObject(process.get(), 0);
    if (waited == WAIT_TIMEOUT) {
      return false;
    }
    if (waited != WAIT_OBJECT_0) {
      return std::unexpected(NativeError("WaitForSingleObject(child)",
                                         waited == WAIT_FAILED ? ::GetLastError() : waited));
    }
    DWORD code = 0;
    if (!::GetExitCodeProcess(process.get(), &code)) {
      return std::unexpected(NativeError("GetExitCodeProcess", ::GetLastError()));
    }
    exit_code = static_cast<std::uint32_t>(code);
    terminal = true;
    return true;
  }

  [[nodiscard]] Result<PipeState> ReadObservation(DWORD maximum) {
    if (pipe_eof) {
      return PipeState::Eof;
    }
    DWORD available = 0;
    if (!::PeekNamedPipe(read.get(), nullptr, 0, nullptr, &available, nullptr)) {
      const DWORD error = ::GetLastError();
      if (error == ERROR_BROKEN_PIPE) {
        pipe_eof = true;
        return PipeState::Eof;
      }
      return std::unexpected(NativeError("PeekNamedPipe", error));
    }
    if (available == 0) {
      return PipeState::Idle;
    }
    if (available > kObservationLimit - observed_bytes) {
      return std::unexpected(Error::Corruption("crash observation exceeds 4096 bytes"));
    }
    std::array<char, 512> buffer{};
    const DWORD requested =
        std::min({available, maximum, static_cast<DWORD>(buffer.size())});
    DWORD received = 0;
    if (!::ReadFile(read.get(), buffer.data(), requested, &received, nullptr)) {
      const DWORD error = ::GetLastError();
      if (error == ERROR_BROKEN_PIPE) {
        pipe_eof = true;
        return PipeState::Eof;
      }
      return std::unexpected(NativeError("ReadFile(observation)", error));
    }
    if (received == 0) {
      return std::unexpected(NativeError("ReadFile(observation zero progress)", ERROR_READ_FAULT));
    }
    if (received > requested) {
      return std::unexpected(NativeError("ReadFile(observation byte count)", ERROR_INVALID_DATA));
    }
    observed_bytes += received;
    Status appended = observation.Append(std::string_view(buffer.data(), received));
    if (!appended.has_value()) {
      Remember(std::move(appended.error()));
    }
    return PipeState::Data;
  }

  [[nodiscard]] Status Pause(Deadline deadline) const {
    const DWORD milliseconds = RemainingMilliseconds(deadline, 5);
    if (terminal) {
      ::Sleep(milliseconds);
      return {};
    }
    const DWORD waited = ::WaitForSingleObject(process.get(), milliseconds);
    if (waited == WAIT_OBJECT_0 || waited == WAIT_TIMEOUT) {
      return {};
    }
    return std::unexpected(NativeError("WaitForSingleObject(child observation)",
                                       waited == WAIT_FAILED ? ::GetLastError() : waited));
  }

  void ValidateExit(bool forced) {
    if (exit_code.has_value() && *exit_code != 0 &&
        !(forced && termination_requested && *exit_code == kForcedExitCode)) {
      Remember(Error::Aborted("crash child exited with code " + std::to_string(*exit_code)));
    }
  }

  [[nodiscard]] Status CompleteAt(Deadline deadline, bool forced) {
    if (completion_observed) {
      ValidateExit(forced);
      return CurrentStatus();
    }
    while (true) {
      if (Clock::now() >= deadline) {
        return Fail(TimeoutError(terminal ? "observation pipe EOF" : "WaitForSingleObject(child)"));
      }
      const auto ended = ObserveTerminal();
      if (!ended.has_value()) {
        return Fail(ended.error());
      }
      const auto pipe = ReadObservation(512);
      if (!pipe.has_value()) {
        return Fail(pipe.error());
      }
      if (terminal && pipe_eof) {
        Status finished = observation.Finish();
        if (!finished.has_value()) {
          Remember(std::move(finished.error()));
        }
        if (!observation.ready()) {
          Remember(Error::Corruption("crash observation ended before readiness"));
        }
        ValidateExit(forced);
        completion_observed = true;
        return CurrentStatus();
      }
      if (*pipe != PipeState::Data) {
        Status paused = Pause(deadline);
        if (!paused.has_value()) {
          return Fail(std::move(paused.error()));
        }
      }
    }
  }

  void Cleanup() noexcept {
    write.CloseAndReport();
    if (process && !terminal) {
      const Deadline deadline = Clock::now() + kCleanupLimit;
      DWORD waited = ::WaitForSingleObject(process.get(), 0);
      if (waited != WAIT_OBJECT_0) {
        if (waited != WAIT_TIMEOUT) {
          ReportCleanupFailure("WaitForSingleObject(child cleanup)",
                               waited == WAIT_FAILED ? ::GetLastError() : waited);
        }
        if (!::TerminateProcess(process.get(), kForcedExitCode)) {
          ReportCleanupFailure("TerminateProcess(child cleanup)", ::GetLastError());
          job.CloseAndReport();
        }
        waited = ::WaitForSingleObject(process.get(), RemainingMilliseconds(deadline, 5'000));
        if (waited != WAIT_OBJECT_0) {
          ReportCleanupFailure("WaitForSingleObject(child cleanup)",
                               waited == WAIT_FAILED ? ::GetLastError() : waited);
          job.CloseAndReport();
          const DWORD remaining = RemainingMilliseconds(deadline, 5'000);
          if (remaining != 0) {
            waited = ::WaitForSingleObject(process.get(), remaining);
            if (waited != WAIT_OBJECT_0) {
              ReportCleanupFailure("WaitForSingleObject(job cleanup)",
                                   waited == WAIT_FAILED ? ::GetLastError() : waited);
            }
          }
        }
      }
      if (waited == WAIT_OBJECT_0) {
        DWORD code = 0;
        if (!::GetExitCodeProcess(process.get(), &code)) {
          ReportCleanupFailure("GetExitCodeProcess(child cleanup)", ::GetLastError());
        } else {
          exit_code = static_cast<std::uint32_t>(code);
          terminal = true;
        }
      }
    }
    thread.CloseAndReport();
    job.CloseAndReport();
    process.CloseAndReport();
    read.CloseAndReport();
  }
};

WindowsCrashProcess::WindowsCrashProcess(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

WindowsCrashProcess::~WindowsCrashProcess() = default;

Result<std::unique_ptr<WindowsCrashProcess>> WindowsCrashProcess::Start(
    const std::filesystem::path& executable, std::wstring_view mode,
    const std::filesystem::path& directory) {
  if (!executable.is_absolute() || !directory.is_absolute() ||
      HasEmbeddedNull(executable.native()) || HasEmbeddedNull(directory.native())) {
    return std::unexpected(Error::InvalidArgument("crash child requires absolute native paths"));
  }
  if (!IsFixedMode(mode)) {
    return std::unexpected(Error::InvalidArgument("unknown crash child mode"));
  }
  std::wstring command = QuoteArgument(executable.native()) + L" " + QuoteArgument(mode) + L" " +
                         QuoteArgument(directory.native());
  if (command.size() >= 32'767) {
    return std::unexpected(Error::InvalidArgument("crash child command line is too long"));
  }
  auto child = std::unique_ptr<WindowsCrashProcess>(
      new WindowsCrashProcess(std::make_unique<Impl>()));
  Impl& impl = *child->impl_;

  HANDLE read = nullptr;
  HANDLE write = nullptr;
  if (!::CreatePipe(&read, &write, nullptr, static_cast<DWORD>(kObservationLimit))) {
    return std::unexpected(NativeError("CreatePipe", ::GetLastError()));
  }
  impl.read.Adopt(read);
  impl.write.Adopt(write);
  if (!::SetHandleInformation(write, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
    return std::unexpected(NativeError("SetHandleInformation(observation write)", ::GetLastError()));
  }

  impl.job.Adopt(::CreateJobObjectW(nullptr, nullptr));
  if (!impl.job) {
    return std::unexpected(NativeError("CreateJobObjectW", ::GetLastError()));
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!::SetInformationJobObject(impl.job.get(), JobObjectExtendedLimitInformation, &limits,
                                 static_cast<DWORD>(sizeof(limits)))) {
    return std::unexpected(NativeError("SetInformationJobObject", ::GetLastError()));
  }

  std::array<HANDLE, 1> inherited_handles{impl.write.get()};
  std::array<HANDLE, 1> jobs{impl.job.get()};
  LaunchAttributes attributes;
  Status initialized = attributes.Initialize();
  if (!initialized.has_value()) {
    return std::unexpected(std::move(initialized.error()));
  }
  if (!::UpdateProcThreadAttribute(attributes.get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited_handles.data(), sizeof(inherited_handles), nullptr,
                                   nullptr)) {
    return std::unexpected(NativeError("UpdateProcThreadAttribute(HANDLE_LIST)", ::GetLastError()));
  }
  if (!::UpdateProcThreadAttribute(attributes.get(), 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, jobs.data(),
                                   sizeof(jobs), nullptr, nullptr)) {
    return std::unexpected(NativeError("UpdateProcThreadAttribute(JOB_LIST)", ::GetLastError()));
  }

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = static_cast<DWORD>(sizeof(startup));
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
  startup.StartupInfo.hStdOutput = impl.write.get();
  startup.StartupInfo.hStdError = impl.write.get();
  startup.lpAttributeList = attributes.get();
  PROCESS_INFORMATION information{};
  if (!::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                         CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                         nullptr, &startup.StartupInfo, &information)) {
    return std::unexpected(NativeError("CreateProcessW", ::GetLastError()));
  }
  impl.process.Adopt(information.hProcess);
  impl.thread.Adopt(information.hThread);
  Status closed_write = impl.write.Close();
  if (!closed_write.has_value()) {
    return std::unexpected(std::move(closed_write.error()));
  }
  const DWORD resumed = ::ResumeThread(impl.thread.get());
  if (resumed == static_cast<DWORD>(-1)) {
    return std::unexpected(NativeError("ResumeThread", ::GetLastError()));
  }
  if (resumed != 1) {
    return std::unexpected(
        Error::Io("ResumeThread returned unexpected suspend count " + std::to_string(resumed)));
  }
  Status closed_thread = impl.thread.Close();
  if (!closed_thread.has_value()) {
    return std::unexpected(std::move(closed_thread.error()));
  }
  return child;
}

Status WindowsCrashProcess::WaitUntilReady(Milliseconds timeout) {
  if (impl_->first_error.has_value()) {
    return impl_->CurrentStatus();
  }
  if (timeout <= Milliseconds::zero()) {
    return impl_->Fail(Error::InvalidArgument("readiness timeout must be positive"));
  }
  const Deadline deadline = Clock::now() + std::min(timeout, kReadinessLimit);
  while (!impl_->observation.ready()) {
    if (Clock::now() >= deadline) {
      return impl_->Fail(TimeoutError("crash child readiness"));
    }
    const auto ended = impl_->ObserveTerminal();
    if (!ended.has_value()) {
      return impl_->Fail(ended.error());
    }
    // Leave everything after the ready newline for terminal EOF validation.
    const auto pipe = impl_->ReadObservation(1);
    if (!pipe.has_value()) {
      return impl_->Fail(pipe.error());
    }
    if (impl_->first_error.has_value()) {
      return impl_->CurrentStatus();
    }
    impl_->ValidateExit(false);
    if (impl_->first_error.has_value()) {
      return impl_->CurrentStatus();
    }
    if (impl_->observation.ready()) {
      return {};
    }
    if (impl_->pipe_eof) {
      Status finished = impl_->observation.Finish();
      if (!finished.has_value()) {
        return impl_->Fail(std::move(finished.error()));
      }
      return impl_->Fail(Error::Corruption("crash observation ended before readiness"));
    }
    if (*pipe != PipeState::Data) {
      Status paused = impl_->Pause(deadline);
      if (!paused.has_value()) {
        return impl_->Fail(std::move(paused.error()));
      }
    }
  }
  return {};
}

Status WindowsCrashProcess::Complete(Milliseconds timeout) {
  if (timeout <= Milliseconds::zero()) {
    return impl_->Fail(Error::InvalidArgument("completion timeout must be positive"));
  }
  return impl_->CompleteAt(Clock::now() + std::min(timeout, kCompletionLimit), false);
}

Status WindowsCrashProcess::TerminateAndComplete(Milliseconds timeout) {
  if (timeout <= Milliseconds::zero()) {
    return impl_->Fail(Error::InvalidArgument("termination timeout must be positive"));
  }
  const Deadline deadline = Clock::now() + std::min(timeout, kCleanupLimit);
  const auto ended = impl_->ObserveTerminal();
  if (!ended.has_value()) {
    impl_->Remember(ended.error());
  }
  if (!ended.has_value() || !*ended) {
    if (!::TerminateProcess(impl_->process.get(), kForcedExitCode)) {
      impl_->Remember(NativeError("TerminateProcess", ::GetLastError()));
    } else {
      impl_->termination_requested = true;
    }
  }
  return impl_->CompleteAt(deadline, true);
}

Result<bool> WindowsCrashProcess::Running() const {
  if (impl_->terminal) {
    return false;
  }
  const DWORD waited = ::WaitForSingleObject(impl_->process.get(), 0);
  if (waited == WAIT_OBJECT_0) {
    return false;
  }
  if (waited == WAIT_TIMEOUT) {
    return true;
  }
  return std::unexpected(NativeError("WaitForSingleObject(child running)",
                                     waited == WAIT_FAILED ? ::GetLastError() : waited));
}

Result<std::uint32_t> WindowsCrashProcess::ExitCode() const {
  if (impl_->terminal) {
    return *impl_->exit_code;
  }
  const DWORD waited = ::WaitForSingleObject(impl_->process.get(), 0);
  if (waited == WAIT_TIMEOUT) {
    return std::unexpected(Error::Busy("crash child has not reached a terminal process state"));
  }
  if (waited != WAIT_OBJECT_0) {
    return std::unexpected(NativeError("WaitForSingleObject(child exit code)",
                                       waited == WAIT_FAILED ? ::GetLastError() : waited));
  }
  DWORD code = 0;
  if (!::GetExitCodeProcess(impl_->process.get(), &code)) {
    return std::unexpected(NativeError("GetExitCodeProcess", ::GetLastError()));
  }
  return static_cast<std::uint32_t>(code);
}

}  // namespace modern_leveldb::test_support
