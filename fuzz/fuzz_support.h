#ifndef MODERN_LEVELDB_FUZZ_SUPPORT_H_
#define MODERN_LEVELDB_FUZZ_SUPPORT_H_

#include <cstdlib>

namespace modern_leveldb::fuzz_support {

inline void Require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

}  // namespace modern_leveldb::fuzz_support

#endif  // MODERN_LEVELDB_FUZZ_SUPPORT_H_
