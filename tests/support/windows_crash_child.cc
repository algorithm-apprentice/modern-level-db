#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>

#include "modern_leveldb/db.h"

namespace {

constexpr UINT kArgumentFailure = 2;
constexpr UINT kOpenFailure = 3;
constexpr UINT kPutFailure = 4;
constexpr UINT kObservationFailure = 5;
constexpr UINT kMalformedExit = 6;
constexpr UINT kExceptionFailure = 7;

bool WriteRecord(HANDLE output, std::string_view record) noexcept {
  while (!record.empty()) {
    const DWORD requested = static_cast<DWORD>(record.size());
    DWORD written = 0;
    if (!::WriteFile(output, record.data(), requested, &written, nullptr) || written == 0 ||
        written > requested) {
      return false;
    }
    record.remove_prefix(written);
  }
  return true;
}

[[noreturn]] void Fail(HANDLE output, UINT exit_code) noexcept {
  static_cast<void>(WriteRecord(output, "failure\n"));
  ::ExitProcess(exit_code);
}

[[noreturn]] void Hold() noexcept {
  for (;;) {
    ::Sleep(60'000);
  }
}

[[noreturn]] void WriteDatabaseAndCrash(HANDLE output, std::wstring_view mode,
                                       const std::filesystem::path& directory) {
  modern_leveldb::Options options;
  options.create_if_missing = true;
  options.allow_weak_namespace_durability = true;
  options.write_buffer_size = 64 * 1'024;
  auto database = modern_leveldb::Database::Open(options, directory);
  if (!database.has_value()) {
    Fail(output, kOpenFailure);
  }
  // Keep the database outside the exception scope and never destroy it in a crash mode.
  try {
    constexpr std::array<std::string_view, 8> acknowledgements{
        "ack 0\n", "ack 1\n", "ack 2\n", "ack 3\n",
        "ack 4\n", "ack 5\n", "ack 6\n", "ack 7\n"};
    const std::string value(80 * 1'024, 'x');
    for (unsigned index = 0; index < acknowledgements.size(); ++index) {
      const std::string key = "key-" + std::to_string(index);
      const modern_leveldb::Status written =
          database->Put(modern_leveldb::AsBytes(key), modern_leveldb::AsBytes(value), {.sync = true});
      if (!written.has_value()) {
        Fail(output, kPutFailure);
      }
      if (!WriteRecord(output, acknowledgements[index])) {
        Fail(output, kObservationFailure);
      }
    }
    if (!WriteRecord(output, "ready\n")) {
      Fail(output, kObservationFailure);
    }
    if (mode == L"hold") {
      Hold();
    }
    if (mode == L"tail" && !WriteRecord(output, "tail\n")) {
      Fail(output, kObservationFailure);
    }
    ::ExitProcess(0);
  } catch (const std::exception&) {
    Fail(output, kExceptionFailure);
  }
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  const HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
  if (output == nullptr || output == INVALID_HANDLE_VALUE) {
    ::ExitProcess(kObservationFailure);
  }
  if (argc != 3) {
    Fail(output, kArgumentFailure);
  }
  const std::wstring_view mode(argv[1]);
  if (mode == L"malformed") {
    if (!WriteRecord(output, "bad\n")) {
      Fail(output, kObservationFailure);
    }
    ::ExitProcess(kMalformedExit);
  }
  if (mode == L"stall") {
    Hold();
  }
  if (mode != L"exit" && mode != L"hold" && mode != L"tail") {
    Fail(output, kArgumentFailure);
  }
  try {
    WriteDatabaseAndCrash(output, mode, std::filesystem::path(argv[2]));
  } catch (const std::exception&) {
    Fail(output, kExceptionFailure);
  }
}
