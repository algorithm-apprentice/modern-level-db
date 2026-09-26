#include "support/crash_file_system.h"

#include <algorithm>
#include <utility>

namespace modern_leveldb::test_support {
namespace {

std::filesystem::path Parent(const std::filesystem::path& path) {
  const auto parent = path.parent_path();
  return parent.empty() ? std::filesystem::path(".") : parent;
}

}  // namespace

class CrashFileSystem::File final : public WritableFile {
 public:
  File(CrashFileSystem& fs, std::filesystem::path path, std::shared_ptr<FileState> state,
       std::unique_ptr<WritableFile> file)
      : fs_(fs), path_(std::move(path)), state_(std::move(state)), file_(std::move(file)) {}

  Status Append(ByteView bytes) override {
    const Status ready = fs_.BeforeMutation();
    if (!ready.has_value()) {
      return ready;
    }
    return file_->Append(bytes);
  }
  Status Flush() override {
    const Status ready = fs_.BeforeMutation();
    if (!ready.has_value()) {
      return ready;
    }
    return file_->Flush();
  }
  Status Close() override {
    const Status ready = fs_.BeforeMutation();
    if (!ready.has_value()) {
      return ready;
    }
    return file_->Close();
  }
  Status Sync() override {
    const Status ready = fs_.BeforeMutation();
    if (!ready.has_value()) {
      return ready;
    }
    const Status synced = file_->Sync();
    if (synced.has_value()) {
      state_->durable = fs_.Contents(path_).value();
    }
    return synced;
  }

 private:
  CrashFileSystem& fs_;
  std::filesystem::path path_;
  std::shared_ptr<FileState> state_;
  std::unique_ptr<WritableFile> file_;
};

CrashFileSystem::CrashFileSystem(const CrashImage& image)
    : live_directories_(image.directories), durable_directories_(image.directories) {
  for (const auto& directory : image.directories) {
    AddDirectory(directory);
  }
  for (const auto& [path, bytes] : image.files) {
    auto state = std::make_shared<FileState>();
    state->durable = bytes;
    live_names_[path] = state;
    durable_names_[path] = std::move(state);
    Write(path, bytes);
  }
}

Status CrashFileSystem::BeforeMutation() {
  if (!crashed_) {
    crashed_ = crash_at_ == mutation_count_;
    ++mutation_count_;
  }
  if (crashed_) {
    return std::unexpected(Error::Io("simulated power loss"));
  }
  return {};
}

CrashImage CrashFileSystem::DurableImage() const {
  CrashImage image;
  image.directories = durable_directories_;
  for (const auto& [path, state] : durable_names_) {
    if (durable_directories_.contains(Parent(path))) {
      image.files.emplace(path, state->durable);
    }
  }
  return image;
}

Result<std::unique_ptr<WritableFile>> CrashFileSystem::OpenWritable(
    const std::filesystem::path& path) {
  const Status ready = BeforeMutation();
  if (!ready.has_value()) {
    return std::unexpected(ready.error());
  }
  auto file = MemoryFileSystem::OpenWritable(path);
  if (!file.has_value()) {
    return std::unexpected(file.error());
  }
  auto state = std::make_shared<FileState>();
  live_names_[path] = state;
  return std::make_unique<File>(*this, path, std::move(state), std::move(*file));
}

Status CrashFileSystem::CreateDirectory(const std::filesystem::path& path) {
  const Status ready = BeforeMutation();
  if (!ready.has_value()) {
    return ready;
  }
  const Status created = MemoryFileSystem::CreateDirectory(path);
  if (created.has_value()) {
    live_directories_.insert(path);
  }
  return created;
}

Status CrashFileSystem::RemoveFile(const std::filesystem::path& path) {
  const Status ready = BeforeMutation();
  if (!ready.has_value()) {
    return ready;
  }
  const Status removed = MemoryFileSystem::RemoveFile(path);
  if (removed.has_value()) {
    live_names_.erase(path);
  }
  return removed;
}

Status CrashFileSystem::RenameFile(const std::filesystem::path& source,
                                   const std::filesystem::path& destination) {
  const Status ready = BeforeMutation();
  if (!ready.has_value()) {
    return ready;
  }
  const Status renamed = MemoryFileSystem::RenameFile(source, destination);
  if (renamed.has_value()) {
    const auto state = live_names_.at(source);
    live_names_.erase(source);
    live_names_[destination] = state;
  }
  return renamed;
}

Status CrashFileSystem::SyncDirectory(const std::filesystem::path& path) {
  const Status ready = BeforeMutation();
  if (!ready.has_value()) {
    return ready;
  }
  const Status synced = MemoryFileSystem::SyncDirectory(path);
  if (!synced.has_value()) {
    return synced;
  }
  std::erase_if(durable_names_, [&](const auto& entry) { return Parent(entry.first) == path; });
  for (const auto& [name, state] : live_names_) {
    if (Parent(name) == path) {
      durable_names_[name] = state;
    }
  }
  std::erase_if(durable_directories_, [&](const auto& name) { return Parent(name) == path; });
  for (const auto& name : live_directories_) {
    if (Parent(name) == path) {
      durable_directories_.insert(name);
    }
  }
  return {};
}

}  // namespace modern_leveldb::test_support
