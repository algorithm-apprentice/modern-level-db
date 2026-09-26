#ifndef MODERN_LEVELDB_TESTS_SUPPORT_TEMPORARY_DIRECTORY_H_
#define MODERN_LEVELDB_TESTS_SUPPORT_TEMPORARY_DIRECTORY_H_

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

namespace modern_leveldb::test_support {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    static std::atomic<unsigned> next{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    do {
      path_ =
          std::filesystem::temp_directory_path() / ("modern-leveldb-test-" + std::to_string(stamp) +
                                                    "-" + std::to_string(next.fetch_add(1)));
    } while (!std::filesystem::create_directory(path_));
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    if (error) {
      std::cerr << "temporary directory cleanup failed: " << error.message() << '\n';
    }
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace modern_leveldb::test_support

#endif  // MODERN_LEVELDB_TESTS_SUPPORT_TEMPORARY_DIRECTORY_H_
