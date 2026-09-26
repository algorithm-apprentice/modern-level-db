#ifndef MODERN_LEVELDB_TESTS_SUPPORT_CRASH_FILE_SYSTEM_H_
#define MODERN_LEVELDB_TESTS_SUPPORT_CRASH_FILE_SYSTEM_H_

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "support/memory_file_system.h"

namespace modern_leveldb::test_support {

struct CrashImage {
  std::set<std::filesystem::path> directories{"."};
  std::map<std::filesystem::path, std::vector<std::byte>> files;
};

// Used on one thread with ManualExecutor. Syncing bytes and syncing names are
// independent, and a simulated crash freezes all subsequent mutations.
class CrashFileSystem final : public MemoryFileSystem {
 public:
  explicit CrashFileSystem(const CrashImage& image = {});

  void CrashAt(std::optional<std::size_t> mutation) { crash_at_ = mutation; }
  [[nodiscard]] bool crashed() const noexcept { return crashed_; }
  [[nodiscard]] std::size_t mutation_count() const noexcept { return mutation_count_; }
  [[nodiscard]] CrashImage DurableImage() const;

  Result<std::unique_ptr<WritableFile>> OpenWritable(const std::filesystem::path& path) override;
  Status CreateDirectory(const std::filesystem::path& path) override;
  Status RemoveFile(const std::filesystem::path& path) override;
  Status RenameFile(const std::filesystem::path& source,
                    const std::filesystem::path& destination) override;
  Status SyncDirectory(const std::filesystem::path& path) override;

 private:
  struct FileState {
    std::vector<std::byte> durable;
  };
  class File;

  Status BeforeMutation();

  std::map<std::filesystem::path, std::shared_ptr<FileState>> live_names_;
  std::map<std::filesystem::path, std::shared_ptr<FileState>> durable_names_;
  std::set<std::filesystem::path> live_directories_;
  std::set<std::filesystem::path> durable_directories_;
  std::optional<std::size_t> crash_at_;
  std::size_t mutation_count_ = 0;
  bool crashed_ = false;
};

}  // namespace modern_leveldb::test_support

#endif  // MODERN_LEVELDB_TESTS_SUPPORT_CRASH_FILE_SYSTEM_H_
