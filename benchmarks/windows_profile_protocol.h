#ifndef MODERN_LEVELDB_BENCHMARKS_WINDOWS_PROFILE_PROTOCOL_H_
#define MODERN_LEVELDB_BENCHMARKS_WINDOWS_PROFILE_PROTOCOL_H_

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace modern_leveldb::profiling::windows {

inline constexpr DWORD ControlMagic = 0x4d4c5052U;
inline constexpr DWORD ControlVersion = 1;
inline constexpr std::size_t ControlBytes = 4096;

struct alignas(8) Control {
  DWORD magic = ControlMagic;
  DWORD version = ControlVersion;
  DWORD pid = 0;
  volatile LONG failure = 0;
  volatile LONG64 epoch = 0;
  volatile LONG active = 0;
};

struct State {
  std::uint64_t epoch;
  bool active;
  DWORD failure;
};

inline State Observe(Control& control) noexcept {
  const LONG64 before = ::InterlockedCompareExchange64(&control.epoch, 0, 0);
  const LONG active = ::InterlockedCompareExchange(&control.active, 0, 0);
  const LONG failure = ::InterlockedCompareExchange(&control.failure, 0, 0);
  const LONG64 after = ::InterlockedCompareExchange64(&control.epoch, 0, 0);
  return {static_cast<std::uint64_t>(after), before == after && active == 1,
          static_cast<DWORD>(failure)};
}

inline std::uint64_t FileTime(FILETIME time) noexcept {
  return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
}

}  // namespace modern_leveldb::profiling::windows

#endif  // MODERN_LEVELDB_BENCHMARKS_WINDOWS_PROFILE_PROTOCOL_H_
