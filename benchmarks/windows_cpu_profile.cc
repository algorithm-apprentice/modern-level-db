#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
// clang-format off
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>
// clang-format on

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "windows_profile_protocol.h"

namespace {
using namespace modern_leveldb::profiling::windows;
using Clock = std::chrono::steady_clock;

[[noreturn]] void Fail(std::string_view operation, DWORD code) {
    throw std::runtime_error(std::string(operation) + " failed (Win32 " + std::to_string(code) +
                             ")");
}
void Check(bool success, std::string_view operation) {
    if (!success) Fail(operation, ::GetLastError());
}

class Handle final {
public:
    Handle() = default;
    explicit Handle(HANDLE value) noexcept : value_(value) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    ~Handle() {
        if (valid() && !::CloseHandle(value_)) {
            std::fprintf(stderr, "native collector cleanup close failed (Win32 %lu)\n",
                         ::GetLastError());
        }
    }
    bool valid() const noexcept { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const noexcept { return value_; }
    void Close() {
        if (valid()) {
            Check(::CloseHandle(value_) != FALSE, "CloseHandle");
            value_ = nullptr;
        }
    }

private:
    HANDLE value_ = nullptr;
};

std::wstring Quote(std::wstring_view argument) {
    std::wstring quoted(1, L'"');
    std::size_t slashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++slashes;
        } else {
            quoted.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            quoted.push_back(character);
            slashes = 0;
        }
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

class Attributes final {
public:
    Attributes(HANDLE job, const std::vector<HANDLE>& handles) {
        SIZE_T size = 0;
        const BOOL queried = ::InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
        const DWORD error = ::GetLastError();
        if (queried || error != ERROR_INSUFFICIENT_BUFFER || size == 0) {
            Fail("InitializeProcThreadAttributeList(size)", error);
        }
        storage_ = std::make_unique<std::byte[]>(size);
        Check(::InitializeProcThreadAttributeList(get(), 2, 0, &size) != FALSE,
              "InitializeProcThreadAttributeList");
        initialized_ = true;
        Check(::UpdateProcThreadAttribute(
                  get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, const_cast<HANDLE*>(handles.data()),
                  handles.size() * sizeof(HANDLE), nullptr, nullptr) != FALSE,
              "UpdateProcThreadAttribute(handles)");
        jobs_[0] = job;
        Check(::UpdateProcThreadAttribute(get(), 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, jobs_.data(),
                                          sizeof(jobs_), nullptr, nullptr) != FALSE,
              "UpdateProcThreadAttribute(job)");
    }
    ~Attributes() {
        if (initialized_) ::DeleteProcThreadAttributeList(get());
    }
    Attributes(const Attributes&) = delete;
    Attributes& operator=(const Attributes&) = delete;
    LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept {
        return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.get());
    }

private:
    std::unique_ptr<std::byte[]> storage_;
    std::array<HANDLE, 1> jobs_{};
    bool initialized_ = false;
};

struct Arguments {
    std::filesystem::path binary, log, report, epochs;
    std::filesystem::path symbols;
    std::vector<std::wstring> child;
    DWORD timeout = 180'000;
    bool sample = false;
    bool allow_zero_cpu = false;
    bool ownership_self_test = false;
};

DWORD Number(std::wstring_view text) {
    if (text.empty() ||
        !std::all_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
        throw std::runtime_error("invalid collector numeric argument");
    unsigned long long value = 0;
    for (wchar_t c : text) {
        if (value > 360'000) throw std::runtime_error("collector timeout exceeds budget");
        value = value * 10 + static_cast<unsigned>(c - L'0');
    }
    if (value == 0 || value > 360'000) throw std::runtime_error("collector timeout out of range");
    return static_cast<DWORD>(value);
}
Arguments Parse(int argc, wchar_t** argv) {
    Arguments result;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view option(argv[index]);
        if (option == L"--") {
            for (++index; index < argc; ++index) result.child.emplace_back(argv[index]);
            break;
        }
        if (option == L"--sample") {
            result.sample = true;
            continue;
        }
        if (option == L"--allow-zero-cpu") {
            result.allow_zero_cpu = true;
            continue;
        }
        if (option == L"--self-test-foreign-thread") {
            result.ownership_self_test = true;
            continue;
        }
        if (++index == argc) throw std::runtime_error("collector option needs value");
        if (option == L"--binary")
            result.binary = argv[index];
        else if (option == L"--log")
            result.log = argv[index];
        else if (option == L"--report")
            result.report = argv[index];
        else if (option == L"--epochs")
            result.epochs = argv[index];
        else if (option == L"--symbols")
            result.symbols = argv[index];
        else if (option == L"--timeout-ms")
            result.timeout = Number(argv[index]);
        else
            throw std::runtime_error("unknown native collector option");
    }
    if (!result.ownership_self_test &&
        (result.binary.empty() || result.log.empty() ||
         (result.sample &&
          (result.report.empty() || result.epochs.empty() || result.symbols.empty())))) {
        throw std::runtime_error("native collector needs binary/log and capture report/epochs");
    }

    return result;
}

bool BelongsToProcess(HANDLE thread, DWORD expected_pid) noexcept {
    return ::GetProcessIdOfThread(thread) == expected_pid;
}

bool ThreadExited(HANDLE thread) {
    const DWORD waited = ::WaitForSingleObject(thread, 0);
    if (waited == WAIT_OBJECT_0) return true;
    if (waited == WAIT_TIMEOUT) return false;
    Fail("WaitForSingleObject(profile thread)", ::GetLastError());
}

void VerifyForeignThreadRejection() {
    Handle thread(::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, ::GetCurrentThreadId()));
    Check(thread.valid(), "OpenThread(self test)");
    const DWORD current = ::GetCurrentProcessId();
    if (!BelongsToProcess(thread.get(), current) ||
        BelongsToProcess(thread.get(), current == MAXDWORD ? current - 1 : current + 1)) {
        throw std::runtime_error("opened-thread ownership validation is incorrect");
    }
    Handle exited(
        ::CreateThread(nullptr, 0, [](LPVOID) -> DWORD { return 0; }, nullptr, 0, nullptr));
    Check(exited.valid(), "CreateThread(self test)");
    Check(::WaitForSingleObject(exited.get(), 5'000) == WAIT_OBJECT_0,
          "WaitForSingleObject(exited thread self test)");
    if (!ThreadExited(exited.get())) {
        throw std::runtime_error("exited-thread race detection is incorrect");
    }
}

// Launches a child already assigned to a kill-on-close job, inheriting only the
// selected handles. Cleanup targets this owned job, never all processes sharing
// the executable name.
class Process final {
public:
    explicit Process(const Arguments& args)
        : deadline_(Clock::now() + std::chrono::milliseconds(args.timeout)),
          job_(::CreateJobObjectW(nullptr, nullptr)) {
        Check(job_.valid(), "CreateJobObjectW");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        Check(::SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits,
                                        sizeof(limits)) != FALSE,
              "SetInformationJobObject");
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        log_ = std::make_unique<Handle>(::CreateFileW(args.log.c_str(), GENERIC_WRITE,
                                                      FILE_SHARE_READ, &security, CREATE_NEW,
                                                      FILE_ATTRIBUTE_NORMAL, nullptr));
        Check(log_->valid(), "CreateFileW(profile log)");
        std::vector<HANDLE> inherited{log_->get()};
        std::vector<std::wstring> arguments(args.child);
        if (args.sample) {
            mapping_ = std::make_unique<Handle>(
                ::CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0,
                                     static_cast<DWORD>(ControlBytes), nullptr));
            Check(mapping_->valid(), "CreateFileMappingW(profile control)");
            control_ = static_cast<Control*>(
                ::MapViewOfFile(mapping_->get(), FILE_MAP_ALL_ACCESS, 0, 0, ControlBytes));
            Check(control_ != nullptr, "MapViewOfFile(profile control)");
            control_->magic = ControlMagic;
            control_->version = ControlVersion;
            control_->pid = 0;
            ::InterlockedExchange(&control_->failure, 0);
            ::InterlockedExchange64(&control_->epoch, 0);
            ::InterlockedExchange(&control_->active, 0);
            ready_ = std::make_unique<Handle>(::CreateEventW(&security, TRUE, FALSE, nullptr));
            proceed_ = std::make_unique<Handle>(::CreateEventW(&security, TRUE, FALSE, nullptr));
            Check(ready_->valid() && proceed_->valid(), "CreateEventW(profile gate)");
            for (HANDLE h : {mapping_->get(), ready_->get(), proceed_->get()})
                inherited.push_back(h);
            const auto argument = [&](std::wstring name, HANDLE h) {
                arguments.push_back(std::move(name));
                arguments.push_back(std::to_wstring(reinterpret_cast<std::uintptr_t>(h)));
            };
            argument(L"--native-profile-control", mapping_->get());
            argument(L"--native-profile-ready", ready_->get());
            argument(L"--native-profile-proceed", proceed_->get());
            arguments.push_back(L"--native-profile-epochs");
            arguments.push_back(args.epochs.native());
        }
        for (HANDLE handle : inherited) {
            DWORD flags = 0;
            Check(::GetHandleInformation(handle, &flags) != FALSE,
                  "GetHandleInformation(inherited)");
            if ((flags & HANDLE_FLAG_INHERIT) == 0) {
                throw std::runtime_error("selectively inherited profile handle is not inheritable");
            }
        }
        Attributes attributes(job_.get(), inherited);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdOutput = log_->get();
        startup.StartupInfo.hStdError = log_->get();
        startup.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
        startup.lpAttributeList = attributes.get();
        std::wstring command = Quote(args.binary.native());
        for (const auto& argument : arguments) command += L" " + Quote(argument);
        PROCESS_INFORMATION information{};
        Check(::CreateProcessW(args.binary.c_str(), command.data(), nullptr, nullptr, TRUE,
                               CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                               nullptr, nullptr, &startup.StartupInfo, &information) != FALSE,
              "CreateProcessW(owned profile)");
        process_ = std::make_unique<Handle>(information.hProcess);
        thread_ = std::make_unique<Handle>(information.hThread);
        pid_ = information.dwProcessId;
        if (control_ != nullptr) control_->pid = pid_;
        Check(::ResumeThread(thread_->get()) != MAXDWORD, "ResumeThread(launch)");
        log_->Close();
    }
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    ~Process() {
        if (!finished_) {
            if (!::TerminateJobObject(job_.get(), 1)) {
                std::fprintf(stderr, "native collector cleanup termination failed (Win32 %lu)\n",
                             ::GetLastError());
            }
            if (process_ && ::WaitForSingleObject(process_->get(), 5'000) != WAIT_OBJECT_0) {
                std::fprintf(stderr, "native collector cleanup reap failed\n");
            }
        }
        if (control_ != nullptr && !::UnmapViewOfFile(control_)) {
            std::fprintf(stderr, "native collector cleanup control unmap failed (Win32 %lu)\n",
                         ::GetLastError());
        }
    }
    HANDLE handle() const { return process_->get(); }
    HANDLE initial_thread() const { return thread_->get(); }
    DWORD pid() const { return pid_; }
    Control& control() { return *control_; }
    bool Exited() {
        const DWORD status = ::WaitForSingleObject(handle(), 0);
        if (status == WAIT_OBJECT_0) return true;
        if (status != WAIT_TIMEOUT) Fail("WaitForSingleObject(profile)", ::GetLastError());
        if (Clock::now() >= deadline_) throw std::runtime_error("owned profile deadline expired");
        return false;
    }
    void Ready() {
        while (!Exited()) {
            const DWORD status = ::WaitForSingleObject(ready_->get(), 10);
            if (status == WAIT_OBJECT_0) return;
            if (status != WAIT_TIMEOUT) Fail("WaitForSingleObject(readiness)", ::GetLastError());
        }
        throw std::runtime_error("owned workload exited before profile readiness");
    }
    void Proceed() { Check(::SetEvent(proceed_->get()) != FALSE, "SetEvent(profile proceed)"); }
    void Finish() {
        DWORD code = 0;
        Check(::GetExitCodeProcess(handle(), &code) != FALSE, "GetExitCodeProcess(profile)");
        if (code != 0)
            throw std::runtime_error("owned workload failed with code " + std::to_string(code));
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION information{};
        Check(::QueryInformationJobObject(job_.get(), JobObjectBasicAccountingInformation,
                                          &information, sizeof(information), nullptr) != FALSE,
              "QueryInformationJobObject(profile)");
        if (information.ActiveProcesses != 0) {
            throw std::runtime_error("owned workload left live descendants");
        }
        finished_ = true;
        process_->Close();
        thread_->Close();
        job_.Close();
    }

private:
    Clock::time_point deadline_;
    Handle job_;
    std::unique_ptr<Handle> process_, thread_, log_, mapping_, ready_, proceed_;
    Control* control_ = nullptr;
    DWORD pid_ = 0;
    bool finished_ = false;
};

void JsonString(std::ostream& out, std::string_view text) {
    constexpr std::string_view Hex = "0123456789abcdef";
    out << '"';
    for (const unsigned char character : text) {
        switch (character) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (character < 0x20U) {
                    out << "\\u00" << Hex[character >> 4U] << Hex[character & 0x0fU];
                } else {
                    out << static_cast<char>(character);
                }
        }
    }
    out << '"';
}

class SuspendedThread final {
public:
    explicit SuspendedThread(HANDLE thread) : thread_(thread) {
        if (::SuspendThread(thread_) == MAXDWORD) {
            error_ = ::GetLastError();
        } else {
            active_ = true;
        }
    }
    SuspendedThread(const SuspendedThread&) = delete;
    SuspendedThread& operator=(const SuspendedThread&) = delete;
    ~SuspendedThread() {
        if (active_ && ::ResumeThread(thread_) == MAXDWORD) {
            std::fprintf(stderr, "native collector cleanup resume failed (Win32 %lu)\n",
                         ::GetLastError());
        }
    }
    DWORD Resume() noexcept {
        if (active_) {
            if (::ResumeThread(thread_) == MAXDWORD) {
                return ::GetLastError();
            }
            active_ = false;
        }
        return ERROR_SUCCESS;
    }
    bool active() const noexcept { return active_; }
    DWORD error() const noexcept { return error_; }
    void Disarm() noexcept { active_ = false; }

private:
    HANDLE thread_;
    bool active_ = false;
    DWORD error_ = ERROR_SUCCESS;
};

struct ProfileFrame {
    std::string module;
    std::string symbol;
    std::uint64_t module_offset = 0;
    bool resolved = false;
};

struct WalkResult {
    std::vector<ProfileFrame> frames;
    std::string status = "frame_limit";
    std::uint64_t repeated_addresses = 0;
    std::uint64_t unresolved_frames = 0;
};

struct StackAggregate {
    std::uint64_t cpu = 0;
    std::uint64_t observations = 0;
    bool attributed = false;
    std::uint64_t epoch = 0;
    std::string unwind_status;
    std::uint64_t unresolved_frames = 0;
    std::uint64_t repeated_addresses = 0;
    std::vector<ProfileFrame> frames;
};

// Readiness/PDB validation -> active-epoch sampling -> retained report. Stacks
// are weighted by per-thread CPU deltas, not wall time. Suspending threads for
// stack walks perturbs execution, so capture is diagnostic, not throughput evidence.
class Sampler final {
public:
    Sampler(Process& process, const Arguments& args)
        : process_(process),
          report_(args.report),
          symbols_(args.symbols),
          allow_zero_cpu_(args.allow_zero_cpu),
          timer_(::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS)) {
        process_.Ready();
        Check(timer_.valid(), "CreateWaitableTimerExW(profile sample)");
        if (!std::filesystem::is_regular_file(args.symbols) ||
            args.symbols.parent_path() != args.binary.parent_path()) {
            throw std::runtime_error("matching PDB must be beside the owned benchmark snapshot");
        }
        ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS |
                        SYMOPT_NO_PROMPTS | SYMOPT_EXACT_SYMBOLS | SYMOPT_IGNORE_CVREC);
        Check(::SymInitializeW(process_.handle(), args.binary.parent_path().c_str(), TRUE) != FALSE,
              "SymInitializeW");
        symbols_initialized_ = true;
        try {
            HMODULE module = nullptr;
            DWORD needed = 0;
            Check(::EnumProcessModulesEx(process_.handle(), &module, sizeof(module), &needed,
                                         LIST_MODULES_64BIT) != FALSE,
                  "EnumProcessModulesEx");
            if (needed < sizeof(module) || module == nullptr) {
                throw std::runtime_error("owned benchmark main module is unavailable");
            }
            MODULEINFO information{};
            Check(::GetModuleInformation(process_.handle(), module, &information,
                                         sizeof(information)) != FALSE,
                  "GetModuleInformation");
            main_base_ = reinterpret_cast<std::uintptr_t>(information.lpBaseOfDll);
            main_end_ = main_base_ + information.SizeOfImage;
            SuspendedThread suspended(process_.initial_thread());
            if (!suspended.active()) {
                Fail("SuspendThread(symbol validation)", suspended.error());
            }
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            Check(::GetThreadContext(process_.initial_thread(), &context) != FALSE,
                  "GetThreadContext(symbol validation)");
            bool ignored = false;
            static_cast<void>(Walk(process_.initial_thread(), context, ignored));
            const DWORD resume_error = suspended.Resume();
            if (resume_error != ERROR_SUCCESS) {
                Fail("ResumeThread(symbol validation)", resume_error);
            }
            if (!pdb_validated_) {
                throw std::runtime_error("owned benchmark PDB did not resolve an owned frame");
            }
        } catch (...) {
            if (!::SymCleanup(process_.handle())) {
                std::fprintf(stderr, "native collector failed symbol cleanup (Win32 %lu)\n",
                             ::GetLastError());
            }
            symbols_initialized_ = false;
            throw;
        }
    }
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;
    ~Sampler() {
        if (symbols_initialized_ && !::SymCleanup(process_.handle())) {
            std::fprintf(stderr, "native collector symbol cleanup failed (Win32 %lu)\n",
                         ::GetLastError());
        }
    }

    void Capture() {
        process_.Proceed();
        constexpr std::array<unsigned, 5> Intervals{5, 7, 11, 13, 17};
        std::size_t interval = 0;
        while (!process_.Exited()) {
            const State state = Observe(process_.control());
            if (state.failure != 0) Fail("owned profile protocol", state.failure);
            if (state.active) Sample(state);
            Sleep(Intervals[interval]);
            interval = (interval + 1) % Intervals.size();
        }
        const State final = Observe(process_.control());
        if (final.failure != 0) Fail("owned profile protocol", final.failure);
    }

    void Write() const {
        if (!pdb_validated_) {
            throw std::runtime_error("native profile did not validate its PDB");
        }
        if (!allow_zero_cpu_ && (total_cpu_ == 0 || attributed_cpu_ == 0 ||
                                 own_observations_ == 0 || observed_epochs_.empty())) {
            throw std::runtime_error("native profile has no useful owned CPU attribution");
        }
        if (std::filesystem::exists(report_)) {
            throw std::runtime_error("native stack report path already exists");
        }
        const auto temporary = report_.native() + L".tmp";
        std::ofstream out(std::filesystem::path(temporary), std::ios::binary);
        if (!out) throw std::runtime_error("could not create native stack report");
        out << "{\"schema_version\":2,\"method\":\"thread-cpu-delta-stackwalk64-v1\","
               "\"sample_interval_ms\":10,"
               "\"sample_schedule\":\"high-resolution-jitter-5-7-11-13-17-v1\",\"pid\":"
            << process_.pid() << ",\"pdb_matched\":true,\"total_cpu_100ns\":" << total_cpu_
            << ",\"attributed_cpu_100ns\":" << attributed_cpu_
            << ",\"unattributed_cpu_100ns\":" << total_cpu_ - attributed_cpu_
            << ",\"stack_observations\":" << observations_
            << ",\"own_frame_observations\":" << own_observations_
            << ",\"dropped_epoch_changes\":" << dropped_epoch_
            << ",\"dropped_thread_races\":" << dropped_thread_
            << ",\"rejected_foreign_threads\":" << rejected_foreign_
            << ",\"total_frames\":" << total_frames_ << ",\"resolved_frames\":" << resolved_frames_
            << ",\"unresolved_frames\":" << unresolved_frames_
            << ",\"repeated_addresses\":" << repeated_addresses_
            << ",\"truncated_stacks\":" << truncated_stacks_ << ",\"observed_epochs\":[";
        bool first = true;
        for (std::uint64_t epoch : observed_epochs_) {
            if (!first) out << ',';
            first = false;
            out << epoch;
        }
        out << "],\"stacks\":[";
        first = true;
        for (const auto& [key, aggregate] : aggregates_) {
            static_cast<void>(key);
            if (!first) out << ',';
            first = false;
            out << "{\"epoch\":" << aggregate.epoch << ",\"cpu_100ns\":" << aggregate.cpu
                << ",\"observations\":" << aggregate.observations
                << ",\"attributed\":" << (aggregate.attributed ? "true" : "false")
                << ",\"unwind_status\":";
            JsonString(out, aggregate.unwind_status);
            out << ",\"unresolved_frames\":" << aggregate.unresolved_frames
                << ",\"repeated_addresses\":" << aggregate.repeated_addresses << ",\"frames\":[";
            for (std::size_t index = 0; index < aggregate.frames.size(); ++index) {
                if (index != 0) out << ',';
                const ProfileFrame& frame = aggregate.frames[index];
                out << "{\"module\":";
                JsonString(out, frame.module);
                out << ",\"symbol\":";
                JsonString(out, frame.symbol);
                out << ",\"module_offset\":" << frame.module_offset
                    << ",\"resolved\":" << (frame.resolved ? "true" : "false") << '}';
            }
            out << "]}";
        }
        out << "]}\n";
        out.close();
        if (!out) throw std::runtime_error("native stack report write failed");
        std::filesystem::rename(std::filesystem::path(temporary), report_);
    }

private:
    using BaselineKey = std::tuple<DWORD, std::uint64_t, std::uint64_t>;

    void Sleep(unsigned milliseconds) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(milliseconds) * 10'000;
        Check(::SetWaitableTimerEx(timer_.get(), &due, 0, nullptr, nullptr, nullptr, 0) != FALSE,
              "SetWaitableTimerEx(profile sample)");
        const DWORD waited = ::WaitForSingleObject(timer_.get(), milliseconds + 5'000);
        if (waited != WAIT_OBJECT_0) {
            Fail("WaitForSingleObject(profile timer)",
                 waited == WAIT_FAILED ? ::GetLastError() : waited);
        }
    }

    std::vector<DWORD> OwnedThreads() {
        Handle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
        Check(snapshot.valid(), "CreateToolhelp32Snapshot");
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        std::vector<DWORD> result;
        if (!::Thread32First(snapshot.get(), &entry)) {
            const DWORD error = ::GetLastError();
            if (error != ERROR_NO_MORE_FILES) Fail("Thread32First", error);
            return result;
        }
        do {
            if (entry.dwSize >= offsetof(THREADENTRY32, th32OwnerProcessID) +
                                    sizeof(entry.th32OwnerProcessID) &&
                entry.th32OwnerProcessID == process_.pid()) {
                result.push_back(entry.th32ThreadID);
            }
            entry.dwSize = sizeof(entry);
        } while (::Thread32Next(snapshot.get(), &entry));
        if (::GetLastError() != ERROR_NO_MORE_FILES) Fail("Thread32Next", ::GetLastError());
        return result;
    }

    WalkResult Walk(HANDLE thread, CONTEXT context, bool& own_leaf) {
        STACKFRAME64 frame{};
        frame.AddrPC = {context.Rip, 0, AddrModeFlat};
        frame.AddrStack = {context.Rsp, 0, AddrModeFlat};
        frame.AddrFrame = {context.Rbp, 0, AddrModeFlat};
        WalkResult result;
        const auto append = [&](std::uintptr_t address, bool leaf) {
            alignas(SYMBOL_INFO) std::array<char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> storage{};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage.data());
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = MAX_SYM_NAME;
            DWORD64 displacement = 0;
            const bool resolved =
                ::SymFromAddr(process_.handle(), address, &displacement, symbol) != FALSE;
            IMAGEHLP_MODULE64 module{};
            module.SizeOfStruct = sizeof(module);
            ProfileFrame profile_frame;
            profile_frame.resolved = resolved;
            if (::SymGetModuleInfo64(process_.handle(), address, &module)) {
                profile_frame.module = module.ModuleName;
                profile_frame.module_offset = address - module.BaseOfImage;
            } else {
                profile_frame.module = "unknown";
            }
            if (resolved) {
                profile_frame.symbol = symbol->Name;
            } else {
                ++result.unresolved_frames;
            }
            result.frames.push_back(std::move(profile_frame));
            const bool own_frame = resolved && address >= main_base_ && address < main_end_;
            if (own_frame && !pdb_validated_) {
                ValidatePdb(address);
            }
            if (leaf) {
                own_leaf = own_frame;
            }
        };
        std::uintptr_t last = static_cast<std::uintptr_t>(frame.AddrPC.Offset);
        if (last != 0) {
            append(last, true);
        }
        for (unsigned index = 1; index != 64; ++index) {
            if (!::StackWalk64(IMAGE_FILE_MACHINE_AMD64, process_.handle(), thread, &frame,
                               &context, nullptr, ::SymFunctionTableAccess64, ::SymGetModuleBase64,
                               nullptr)) {
                result.status = "terminated";
                break;
            }
            const std::uintptr_t address = static_cast<std::uintptr_t>(frame.AddrPC.Offset);
            if (address == 0) {
                result.status = "zero_pc";
                break;
            }
            if (address == last) {
                ++result.repeated_addresses;
                continue;
            }
            last = address;
            append(address, false);
        }
        return result;
    }

    void ValidatePdb(std::uintptr_t address) {
        IMAGEHLP_MODULEW64 module{};
        module.SizeOfStruct = sizeof(module);
        Check(::SymGetModuleInfoW64(process_.handle(), address, &module) != FALSE,
              "SymGetModuleInfoW64");
        const bool loaded_name = module.LoadedPdbName[0] != L'\0';
        const bool expected =
            loaded_name &&
            std::filesystem::equivalent(symbols_, std::filesystem::path(module.LoadedPdbName));
        if (module.SymType != SymPdb || module.PdbUnmatched || !expected) {
            throw std::runtime_error("owned benchmark PDB identity is missing or mismatched");
        }
        pdb_validated_ = true;
    }

    void Sample(const State& expected) {
        for (DWORD id : OwnedThreads()) {
            Handle thread(::OpenThread(
                THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | SYNCHRONIZE,
                FALSE, id));
            if (!thread.valid()) {
                const DWORD error = ::GetLastError();
                if (error == ERROR_INVALID_PARAMETER) {
                    ++dropped_thread_;
                    continue;
                }
                Fail("OpenThread(profile sample)", error);
            }
            if (!BelongsToProcess(thread.get(), process_.pid())) {
                ++rejected_foreign_;
                continue;
            }
            FILETIME created{}, exited{}, kernel{}, user{};
            if (!::GetThreadTimes(thread.get(), &created, &exited, &kernel, &user)) {
                const DWORD error = ::GetLastError();
                if (ThreadExited(thread.get())) {
                    ++dropped_thread_;
                    continue;
                }
                Fail("GetThreadTimes(profile sample)", error);
            }
            const std::uint64_t creation = FileTime(created);
            const std::uint64_t cpu = FileTime(kernel) + FileTime(user);
            // Creation time separates reused thread IDs; epoch separates measured
            // intervals so setup or an earlier interval cannot share a CPU baseline.
            const BaselineKey key{id, creation, expected.epoch};
            const auto [position, inserted] = baselines_.try_emplace(key, cpu);
            if (inserted) continue;
            if (cpu < position->second) throw std::runtime_error("thread CPU time moved backwards");
            const std::uint64_t delta = cpu - position->second;
            position->second = cpu;
            if (delta == 0) continue;
            const State before = Observe(process_.control());
            if (!before.active || before.epoch != expected.epoch) {
                ++dropped_epoch_;
                continue;
            }
            SuspendedThread suspended(thread.get());
            if (!suspended.active()) {
                if (ThreadExited(thread.get())) {
                    ++dropped_thread_;
                    continue;
                }
                Fail("SuspendThread(profile sample)", suspended.error());
            }
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (!::GetThreadContext(thread.get(), &context)) {
                const DWORD context_error = ::GetLastError();
                const DWORD resume_error = suspended.Resume();
                if (ThreadExited(thread.get())) {
                    suspended.Disarm();
                    ++dropped_thread_;
                    continue;
                }
                if (resume_error != ERROR_SUCCESS) {
                    Fail("ResumeThread(profile sample)", resume_error);
                }
                Fail("GetThreadContext(profile sample)", context_error);
            }
            bool own_leaf = false;
            WalkResult walk = Walk(thread.get(), context, own_leaf);
            const DWORD resume_error = suspended.Resume();
            if (resume_error != ERROR_SUCCESS) {
                if (ThreadExited(thread.get())) {
                    suspended.Disarm();
                    ++dropped_thread_;
                    continue;
                }
                Fail("ResumeThread(profile sample)", resume_error);
            }
            const State after = Observe(process_.control());
            if (!after.active || after.epoch != expected.epoch) {
                ++dropped_epoch_;
                continue;
            }
            if (walk.frames.empty()) {
                ++dropped_thread_;
                continue;
            }
            if (walk.status == "frame_limit") {
                ++truncated_stacks_;
                continue;
            }
            total_cpu_ += delta;
            attributed_cpu_ += own_leaf ? delta : 0;
            observations_++;
            own_observations_ += own_leaf ? 1 : 0;
            observed_epochs_.insert(expected.epoch);
            std::string aggregate_key = std::to_string(expected.epoch) + (own_leaf ? ":1:" : ":0:");
            aggregate_key += walk.status + ':' + std::to_string(walk.repeated_addresses) + ':';
            for (const auto& frame : walk.frames) {
                aggregate_key += frame.module + ':' + frame.symbol + ':' +
                                 std::to_string(frame.module_offset) + ':' +
                                 std::to_string(frame.resolved) + '\n';
            }
            total_frames_ += walk.frames.size();
            resolved_frames_ += static_cast<std::uint64_t>(
                std::count_if(walk.frames.begin(), walk.frames.end(),
                              [](const ProfileFrame& frame) { return frame.resolved; }));
            unresolved_frames_ += walk.unresolved_frames;
            repeated_addresses_ += walk.repeated_addresses;
            auto& aggregate = aggregates_[aggregate_key];
            if (aggregate.observations == 0) {
                aggregate.epoch = expected.epoch;
                aggregate.attributed = own_leaf;
                aggregate.unwind_status = std::move(walk.status);
                aggregate.unresolved_frames = walk.unresolved_frames;
                aggregate.repeated_addresses = walk.repeated_addresses;
                aggregate.frames = std::move(walk.frames);
            }
            aggregate.cpu += delta;
            aggregate.observations++;
        }
    }

    Process& process_;
    std::filesystem::path report_;
    std::filesystem::path symbols_;
    bool allow_zero_cpu_;
    Handle timer_;
    bool symbols_initialized_ = false;
    bool pdb_validated_ = false;
    std::uintptr_t main_base_ = 0, main_end_ = 0;
    std::map<BaselineKey, std::uint64_t> baselines_;
    std::map<std::string, StackAggregate> aggregates_;
    std::set<std::uint64_t> observed_epochs_;
    std::uint64_t total_cpu_ = 0, attributed_cpu_ = 0, observations_ = 0;
    std::uint64_t own_observations_ = 0, dropped_epoch_ = 0, dropped_thread_ = 0;
    std::uint64_t rejected_foreign_ = 0;
    std::uint64_t total_frames_ = 0, resolved_frames_ = 0, unresolved_frames_ = 0;
    std::uint64_t repeated_addresses_ = 0, truncated_stacks_ = 0;
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const auto args = Parse(argc, argv);
        if (args.ownership_self_test) {
            VerifyForeignThreadRejection();
            return 0;
        }
        Process process(args);
        if (args.sample) {
            {
                Sampler sampler(process, args);
                sampler.Capture();
                sampler.Write();
            }
        } else {
            while (!process.Exited()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        process.Finish();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "native profile collector failed: %s\n", error.what());
        return 1;
    }
}
