#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#include "windows_profile_workload.h"

namespace {
using Clock = std::chrono::steady_clock;
using modern_leveldb::profiling::windows::Workload;

struct Arguments {
  std::string mapping, ready, proceed;
  std::filesystem::path epochs;
  std::string mode;
  unsigned duration = 1'000;
};

unsigned Number(std::string_view text) {
  unsigned value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < 200 ||
      value > 10'000) {
    throw std::runtime_error("calibration duration must be in [200, 10000] ms");
  }
  return value;
}

Arguments Parse(int argc, char** argv) {
  Arguments result;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if (++index == argc) throw std::runtime_error("calibration option needs value");
    if (option == "--native-profile-control")
      result.mapping = argv[index];
    else if (option == "--native-profile-ready")
      result.ready = argv[index];
    else if (option == "--native-profile-proceed")
      result.proceed = argv[index];
    else if (option == "--native-profile-epochs")
      result.epochs = argv[index];
    else if (option == "--mode")
      result.mode = argv[index];
    else if (option == "--duration-ms")
      result.duration = Number(argv[index]);
    else
      throw std::runtime_error("unknown calibration option");
  }
  if (result.mapping.empty() || result.ready.empty() || result.proceed.empty() ||
      result.epochs.empty() ||
      (result.mode != "hot" && result.mode != "wait" && result.mode != "alternating" &&
       result.mode != "alternating-offset" && result.mode != "stall")) {
    throw std::runtime_error("calibration requires the owned protocol and a valid mode");
  }
  return result;
}

__declspec(noinline) std::uint64_t Hot(std::uint64_t value) noexcept {
  for (unsigned index = 0; index != 10'000; ++index) {
    value ^= value >> 12U;
    value ^= value << 25U;
    value ^= value >> 27U;
    value *= 2'685'821'657'736'338'717ULL;
  }
  return value;
}

void Run(const Arguments& args) {
  if (args.mode == "stall") {
    ::Sleep(args.duration);
    return;
  }
  Workload workload;
  workload.Initialize(args.mapping, args.ready, args.proceed, args.epochs);
  workload.Begin(1);
  const auto end = Clock::now() + std::chrono::milliseconds(args.duration);
  std::uint64_t value = 1;
  if (args.mode == "alternating-offset") {
    ::Sleep(5);
  }
  while (Clock::now() < end) {
    if (args.mode == "wait") {
      ::Sleep(10);
    } else if (args.mode == "hot") {
      value = Hot(value);
    } else {
      const auto hot_end = Clock::now() + std::chrono::milliseconds(9);
      while (Clock::now() < hot_end) value = Hot(value);
      ::Sleep(1);
    }
  }
  workload.End(1);
  workload.Finish();
  if (value == 0) throw std::runtime_error("calibration hot loop produced impossible zero");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Run(Parse(argc, argv));
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "native profile calibration failed: %s\n", error.what());
    return 1;
  }
}
