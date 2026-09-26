#include <benchmark/benchmark.h>
#include <leveldb/db.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "profiling_build.h"

#if MODERN_LEVELDB_PROFILE_MARKERS
#include <os/log.h>
#include <os/signpost.h>
#endif

#include "modern_leveldb/base/crc32c.h"
#include "modern_leveldb/db.h"

namespace modern_leveldb::profiling {
namespace {

constexpr std::array<std::string_view, 2> Engines{"modern", "leveldb"};
constexpr std::array<std::string_view, 4> Workloads{"readrandom", "readmissing", "scan",
                                                    "seek_reuse"};
constexpr std::array<std::size_t, 2> RecordCounts{4096, 65536};
constexpr std::size_t ValueSize = 256;

struct Case {
  std::string name;
  std::string engine;
  std::string workload;
  std::size_t records;
};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

void Check(const Status& status) {
  if (!status.has_value()) {
    throw std::runtime_error(status.error().ToString());
  }
}

void Check(const leveldb::Status& status) {
  if (!status.ok()) {
    throw std::runtime_error(status.ToString());
  }
}

template <typename T>
T Take(Result<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error(result.error().ToString());
  }
  return std::move(*result);
}

template <typename Function>
void ForEachCase(Function function) {
  for (const auto engine : Engines) {
    for (const auto workload : Workloads) {
      for (const auto records : RecordCounts) {
        const std::string name =
            std::string(engine) + "/" + std::string(workload) + "/" + std::to_string(records);
        function(Case{name, std::string(engine), std::string(workload), records});
      }
    }
  }
}

Case FindCase(std::string_view name) {
  std::optional<Case> selected;
  ForEachCase([&](const Case& candidate) {
    if (candidate.name == name) {
      selected = candidate;
    }
  });
  Require(selected.has_value(), "unknown performance case; use --list-cases");
  return *selected;
}

struct Arguments {
  std::string case_name;
  std::filesystem::path database;
  std::filesystem::path completion;
  bool profile = false;
  bool list = false;
  bool help = false;
  std::vector<char*> framework;
};

Arguments ParseArguments(int argc, char** argv) {
  Arguments args;
  args.framework.push_back(argv[0]);
  for (int index = 1; index < argc; ++index) {
    const std::string_view option = argv[index];
    if (option == "--case" || option == "--database" || option == "--completion-report") {
      Require(index + 1 < argc, "performance option needs a value");
      const char* value = argv[++index];
      if (option == "--case") {
        Require(args.case_name.empty(), "duplicate --case");
        args.case_name = value;
      } else if (option == "--database") {
        Require(args.database.empty(), "duplicate --database");
        args.database = value;
      } else {
        Require(args.completion.empty(), "duplicate --completion-report");
        args.completion = value;
      }
    } else if (option == "--profile-markers") {
      args.profile = true;
    } else if (option == "--list-cases") {
      args.list = true;
    } else if (option == "--help") {
      args.help = true;
    } else {
      args.framework.push_back(argv[index]);
    }
  }
  return args;
}

std::string Key(std::size_t number) {
  std::string key = std::to_string(number);
  key.insert(0, 10 - key.size(), '0');
  key.insert(0, 1, 'k');
  return key;
}

struct Fingerprint {
  std::uint32_t crc = 0;

  void Bytes(ByteView bytes) { crc = ExtendCrc32c(crc, bytes); }
  void Integer(std::uint64_t value) {
    std::array<std::byte, 8> encoded{};
    for (std::size_t index = 0; index < encoded.size(); ++index) {
      encoded[index] = static_cast<std::byte>((value >> (index * 8U)) & 255U);
    }
    Bytes(encoded);
  }
  void Field(std::string_view bytes) {
    Integer(bytes.size());
    Bytes(AsBytes(bytes));
  }
};

std::vector<std::size_t> Permutation(std::size_t count, std::uint64_t seed) {
  std::vector<std::size_t> result(count);
  std::iota(result.begin(), result.end(), std::size_t{0});
  std::mt19937_64 random(seed);
  for (std::size_t size = count; size > 1; --size) {
    const auto bound = static_cast<std::uint64_t>(size);
    const std::uint64_t threshold = (std::uint64_t{0} - bound) % bound;
    std::uint64_t draw;
    do {
      draw = random();
    } while (draw < threshold);
    std::swap(result[size - 1], result[static_cast<std::size_t>(draw % bound)]);
  }
  return result;
}

std::uint32_t OrderFingerprint(std::string_view domain, const std::vector<std::size_t>& order) {
  Fingerprint result;
  result.Bytes(AsBytes(domain));
  result.Integer(order.size());
  for (const auto index : order) {
    result.Integer(index);
  }
  return result.crc;
}

struct Record {
  std::string key;
  std::string value;
};

struct Corpus {
  explicit Corpus(std::size_t count)
      : insertion(Permutation(count, 302)),
        present(Permutation(count, 303)),
        missing(Permutation(count - 1, 304)) {
    std::mt19937_64 random(301);
    Fingerprint data;
    data.Bytes(AsBytes("modern-perf-records-v1"));
    data.Integer(count);
    records.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      Record record{Key(index * 2), std::string(ValueSize, static_cast<char>('a' + index % 26))};
      if (index % 4 == 0) {
        for (char& byte : record.value) {
          byte = std::bit_cast<char>(static_cast<unsigned char>(random() & 255U));
        }
      }
      data.Field(record.key);
      data.Field(record.value);
      records.push_back(std::move(record));
      if (index + 1 < count) {
        missing_keys.push_back(Key(index * 2 + 1));
      }
    }
    fingerprints = {data.crc, OrderFingerprint("modern-perf-insert-v1", insertion),
                    OrderFingerprint("modern-perf-present-v1", present),
                    OrderFingerprint("modern-perf-missing-v1", missing)};
  }

  std::vector<Record> records;
  std::vector<std::string> missing_keys;
  std::vector<std::size_t> insertion;
  std::vector<std::size_t> present;
  std::vector<std::size_t> missing;
  std::array<std::uint32_t, 4> fingerprints{};
};

class Modern final {
 public:
  using Cursor = Iterator;

  explicit Modern(const std::filesystem::path& path) : database_(Open(path)) {}
  void Put(const Record& record) {
    Check(database_.Put(AsBytes(record.key), AsBytes(record.value)));
  }
  void Read(std::string_view key, bool present) {
    auto value = Take(database_.Get(AsBytes(key)));
    Require(value.has_value() == present, "Modern LevelDB returned incorrect key presence");
    benchmark::DoNotOptimize(value);
  }
  Cursor NewIterator(bool fill_cache) {
    return Take(database_.NewIterator({.fill_cache = fill_cache}));
  }
  static void First(Cursor& cursor) { Check(cursor.SeekToFirst()); }
  static void Next(Cursor& cursor) { Check(cursor.Next()); }
  static void Seek(Cursor& cursor, std::string_view key) { Check(cursor.Seek(AsBytes(key))); }
  static bool Valid(const Cursor& cursor) { return cursor.valid(); }
  static std::string_view CursorKey(const Cursor& cursor) { return AsStringView(cursor.key()); }
  static std::string_view CursorValue(const Cursor& cursor) { return AsStringView(cursor.value()); }

 private:
  static Database Open(const std::filesystem::path& path) {
    Options options;
    options.create_if_missing = true;
    options.write_buffer_size = 64 * 1024;
    options.block_size = 4096;
    options.block_restart_interval = 16;
    options.compression = Compression::Snappy;
    return Take(Database::Open(options, path));
  }
  Database database_;
};

class Reference final {
 public:
  using Cursor = std::unique_ptr<leveldb::Iterator>;

  explicit Reference(const std::filesystem::path& path) {
    leveldb::Options options;
    options.create_if_missing = true;
    options.write_buffer_size = 64 * 1024;
    options.block_size = 4096;
    options.block_restart_interval = 16;
    options.compression = leveldb::kSnappyCompression;
    leveldb::DB* opened = nullptr;
    const auto status = leveldb::DB::Open(options, path.string(), &opened);
    database_.reset(opened);
    Check(status);
  }
  void Put(const Record& record) { Check(database_->Put({}, record.key, record.value)); }
  void Read(std::string_view key, bool present) {
    leveldb::ReadOptions options;
    options.verify_checksums = true;
    const auto status = database_->Get(options, Slice(key), &read_value_);
    if (present || !status.IsNotFound()) {
      Check(status);
    }
    Require(status.ok() == present, "LevelDB reference returned incorrect key presence");
    benchmark::DoNotOptimize(read_value_);
  }
  Cursor NewIterator(bool fill_cache) {
    leveldb::ReadOptions options;
    options.verify_checksums = true;
    options.fill_cache = fill_cache;
    return Cursor(database_->NewIterator(options));
  }
  static void First(Cursor& cursor) {
    cursor->SeekToFirst();
    Check(cursor->status());
  }
  static void Next(Cursor& cursor) {
    cursor->Next();
    Check(cursor->status());
  }
  static void Seek(Cursor& cursor, std::string_view key) {
    cursor->Seek(Slice(key));
    Check(cursor->status());
  }
  static bool Valid(const Cursor& cursor) { return cursor->Valid(); }
  static std::string_view CursorKey(const Cursor& cursor) {
    const auto key = cursor->key();
    return {key.data(), key.size()};
  }
  static std::string_view CursorValue(const Cursor& cursor) {
    const auto value = cursor->value();
    return {value.data(), value.size()};
  }

 private:
  static leveldb::Slice Slice(std::string_view value) { return {value.data(), value.size()}; }
  std::unique_ptr<leveldb::DB> database_;
  std::string read_value_;
};

class ProfileInterval final {
 public:
  ProfileInterval(bool enabled, const std::string& name, benchmark::State& state)
      : enabled_(enabled), name_(name), state_(state) {
#if MODERN_LEVELDB_PROFILE_MARKERS
    if (enabled_) {
      log_ = os_log_create("modern_leveldb.profiling", OS_LOG_CATEGORY_POINTS_OF_INTEREST);
      identifier_ = os_signpost_id_generate(log_);
      os_signpost_interval_begin(log_, identifier_, "workload",
                                 "case=%{public}s iterations=%{public}llu", name_.c_str(),
                                 static_cast<unsigned long long>(state_.max_iterations));
    }
#endif
  }
  ~ProfileInterval() {
#if MODERN_LEVELDB_PROFILE_MARKERS
    if (enabled_) {
      os_signpost_interval_end(log_, identifier_, "workload",
                               "case=%{public}s iterations=%{public}llu", name_.c_str(),
                               static_cast<unsigned long long>(state_.iterations()));
      os_release(log_);
    }
#else
    static_cast<void>(enabled_);
    static_cast<void>(name_);
    static_cast<void>(state_);
#endif
  }
  ProfileInterval(const ProfileInterval&) = delete;
  ProfileInterval& operator=(const ProfileInterval&) = delete;

 private:
  bool enabled_;
  const std::string& name_;
  benchmark::State& state_;
#if MODERN_LEVELDB_PROFILE_MARKERS
  os_log_t log_ = nullptr;
  os_signpost_id_t identifier_ = OS_SIGNPOST_ID_INVALID;
#endif
};

template <typename Adapter>
class Fixture final {
 public:
  Fixture(const Case& selected, const std::filesystem::path& path)
      : selected_(selected), corpus_(selected.records) {
    Require(!std::filesystem::exists(path) && !std::filesystem::is_symlink(path),
            "performance database path must not already exist");
    database_ = std::make_unique<Adapter>(path);
    for (const auto index : corpus_.insertion) {
      database_->Put(corpus_.records[index]);
    }
    database_.reset();
    database_ = std::make_unique<Adapter>(path);
    Verify();
    if (selected_.workload == "seek_reuse") {
      retained_.emplace(database_->NewIterator(true));
      retained_iterators_ = 1;
    }
    warmup_operations_ = selected_.workload == "scan" ? 1 : QueryCount();
    for (std::size_t index = 0; index < warmup_operations_; ++index) {
      Operation();
    }
  }

  void ResetCursor() {
    cursor_ = 0;
    ++cursor_resets_;
    if (retained_.has_value()) {
      Adapter::First(*retained_);
    }
  }

  [[gnu::noinline]] void RunReadRandom(benchmark::State& state) {
    for (auto ignored : state) {
      static_cast<void>(ignored);
      ReadPresent();
    }
  }
  [[gnu::noinline]] void RunReadMissing(benchmark::State& state) {
    for (auto ignored : state) {
      static_cast<void>(ignored);
      ReadMissing();
    }
  }
  [[gnu::noinline]] void RunScan(benchmark::State& state) {
    for (auto ignored : state) {
      static_cast<void>(ignored);
      Scan(false);
    }
  }
  [[gnu::noinline]] void RunSeekReuse(benchmark::State& state) {
    for (auto ignored : state) {
      static_cast<void>(ignored);
      Seek();
    }
  }

  void Run(benchmark::State& state, bool profile) {
    ResetCursor();
    {
      ProfileInterval interval(profile, selected_.name, state);
      if (selected_.workload == "readrandom") {
        RunReadRandom(state);
      } else if (selected_.workload == "readmissing") {
        RunReadMissing(state);
      } else if (selected_.workload == "scan") {
        RunScan(state);
      } else {
        RunSeekReuse(state);
      }
    }
    const std::int64_t items =
        selected_.workload == "scan" ? static_cast<std::int64_t>(selected_.records) : 1;
    state.SetItemsProcessed(state.iterations() * items);
    state.counters["items_per_iteration"] = static_cast<double>(items);
  }

  void Finish() {
    retained_.reset();
    Verify();
    database_.reset();
  }

  void WriteCompletion(const std::filesystem::path& path, std::size_t invocations) const {
    Require(!std::filesystem::exists(path) && !std::filesystem::is_symlink(path),
            "completion report must not already exist");
    std::ofstream output(path);
    output << "{\"schema_version\":1,\"case\":\"" << selected_.name
           << "\",\"preparations\":1,\"verifications\":" << verifications_
           << ",\"callback_invocations\":" << invocations << ",\"cursor_resets\":" << cursor_resets_
           << ",\"warmup_operations\":" << warmup_operations_
           << ",\"retained_iterators\":" << retained_iterators_
           << ",\"scan_creations\":" << scan_creations_
           << ",\"scan_destructions\":" << scan_destructions_;
    constexpr std::array Names{"record_crc32c", "insertion_crc32c", "present_crc32c",
                               "missing_crc32c"};
    for (std::size_t index = 0; index < Names.size(); ++index) {
      output << ",\"" << Names[index] << "\":\"" << std::hex << std::setfill('0') << std::setw(8)
             << corpus_.fingerprints[index] << '"';
    }
    output << "}\n";
    output.close();
    Require(output.good(), "failed to write the completion report");
  }

 private:
  std::size_t QueryCount() const {
    return selected_.workload == "readmissing" ? corpus_.missing.size() : corpus_.present.size();
  }
  std::size_t NextIndex(const std::vector<std::size_t>& order) {
    const std::size_t index = order[cursor_];
    cursor_ = cursor_ + 1 == order.size() ? 0 : cursor_ + 1;
    return index;
  }
  void ReadPresent() { database_->Read(corpus_.records[NextIndex(corpus_.present)].key, true); }
  void ReadMissing() { database_->Read(corpus_.missing_keys[NextIndex(corpus_.missing)], false); }
  void Seek() {
    const std::string& key = corpus_.records[NextIndex(corpus_.present)].key;
    Adapter::Seek(*retained_, key);
    Require(Adapter::Valid(*retained_), "seek returned an invalid iterator");
    Require(Adapter::CursorKey(*retained_) == key, "seek returned the wrong key");
  }
  void Scan(bool verify) {
    ++scan_creations_;
    {
      auto iterator = database_->NewIterator(!verify);
      Adapter::First(iterator);
      std::size_t count = 0;
      while (Adapter::Valid(iterator)) {
        Require(count < corpus_.records.size(), "scan yielded too many records");
        if (verify) {
          Require(Adapter::CursorKey(iterator) == corpus_.records[count].key,
                  "verification found an incorrect key or order");
          Require(Adapter::CursorValue(iterator) == corpus_.records[count].value,
                  "verification found an incorrect value");
        } else {
          auto key = Adapter::CursorKey(iterator);
          auto value = Adapter::CursorValue(iterator);
          benchmark::DoNotOptimize(key);
          benchmark::DoNotOptimize(value);
        }
        ++count;
        Adapter::Next(iterator);
      }
      Require(count == corpus_.records.size(), "scan lost records");
    }
    ++scan_destructions_;
  }
  void Verify() {
    Scan(true);
    ++verifications_;
  }
  void Operation() {
    if (selected_.workload == "readrandom") {
      ReadPresent();
    } else if (selected_.workload == "readmissing") {
      ReadMissing();
    } else if (selected_.workload == "scan") {
      Scan(false);
    } else {
      Seek();
    }
  }

  Case selected_;
  Corpus corpus_;
  std::unique_ptr<Adapter> database_;
  std::optional<typename Adapter::Cursor> retained_;
  std::size_t cursor_ = 0;
  std::size_t cursor_resets_ = 0;
  std::size_t warmup_operations_ = 0;
  std::size_t retained_iterators_ = 0;
  std::size_t scan_creations_ = 0;
  std::size_t scan_destructions_ = 0;
  std::size_t verifications_ = 0;
};

class StatusReporter final : public benchmark::ConsoleReporter {
 public:
  void ReportRuns(const std::vector<Run>& reports) override {
    for (const auto& report : reports) {
      failed = failed || report.skipped != 0;
    }
    ConsoleReporter::ReportRuns(reports);
  }
  bool failed = false;
};

void AddContext(const Case& selected) {
  const auto add = [](std::string_view key, std::string_view value) {
    benchmark::AddCustomContext(std::string(key), std::string(value));
  };
  add("profile_case", selected.name);
  add("engine", selected.engine);
  add("workload", selected.workload);
  add("records", std::to_string(selected.records));
  add("value_bytes", "256");
  add("cache_bytes", "8388608");
  add("write_buffer_bytes", "65536");
  add("block_bytes", "4096");
  add("block_restart_interval", "16");
  add("filter_policy", "none");
  add("compression", "snappy");
  add("wal", "enabled");
  add("preparation_sync", "false");
  add("read_checksums", "true");
  add("timing", "wall_and_process_cpu");
  add("background_quiescence_forced", "false");
  add("source_directory", SourceDirectory);
  add("build_directory", BuildDirectory);
  add("configure_revision", ConfigureRevision);
  add("configure_dirty", ConfigureDirty);
  add("build_type", BuildType);
  add("compiler", Compiler);
  add("c_flags", CFlags);
  add("cxx_flags", CxxFlags);
  add("benchmark_requested_revision", "192ef10025eb2c4cdd392bc502f0c852196baa48");
  add("benchmark_source_override", BenchmarkOverride);
  add("reference_requested_revision", "7ee830d02b623e8ffe0b95d59a74db1e58da04c5");
  add("reference_source_override", ReferenceOverride);
  add("reference_hardware_crc", "disabled");
  add("snappy_target", SnappyTarget);
  add("snappy_source", SnappySource);
  add("snappy_source_override", SnappyOverride);
  add("zstd_target", ZstdTarget);
  add("zstd_source", ZstdSource);
  add("zstd_source_override", ZstdOverride);
  add("crc32c_target", Crc32cTarget);
  add("crc32c_provider", Crc32cProvider);
  add("crc32c_source", Crc32cSource);
  add("crc32c_source_override", Crc32cOverride);
  add("crc32c_requested_revision", Crc32cRequestedRevision);
  add("crc32c_compiled_arm64", Crc32cArm64);
  add("crc32c_compiled_sse42", Crc32cSse42);
#if MODERN_LEVELDB_PROFILE_MARKERS
  add("profile_capture_supported", "true");
#else
  add("profile_capture_supported", "false");
#endif
}

template <typename Adapter>
int RunCase(const Case& selected, const Arguments& args) {
  std::unique_ptr<Fixture<Adapter>> fixture;
  bool failed = false;
  std::size_t invocations = 0;
  benchmark::RegisterBenchmark(selected.name.c_str(),
                               [&](benchmark::State& state) {
                                 ++invocations;
                                 if (failed) {
                                   state.SkipWithError("an earlier invocation failed");
                                   return;
                                 }
                                 try {
                                   if (fixture == nullptr) {
                                     fixture = std::make_unique<Fixture<Adapter>>(selected,
                                                                                  args.database);
                                   }
                                   fixture->Run(state, args.profile);
                                 } catch (const std::exception& error) {
                                   failed = true;
                                   state.SkipWithError(error.what());
                                 }
                               })
      ->UseRealTime()
      ->MeasureProcessCPUTime()
      ->Unit(benchmark::kNanosecond);
  StatusReporter reporter;
  const std::size_t matched = benchmark::RunSpecifiedBenchmarks(&reporter);
  Require(matched == 1, "the framework did not match exactly one case");
  Require(!failed && !reporter.failed, "performance workload failed");
  Require(fixture != nullptr, "no workload executed; use --list-cases to list cases");
  fixture->Finish();
  fixture->WriteCompletion(args.completion, invocations);
  benchmark::Shutdown();
  return 0;
}

int Main(int argc, char** argv) {
  Arguments args = ParseArguments(argc, argv);
  if (args.list) {
    ForEachCase([](const Case& selected) { std::cout << selected.name << '\n'; });
    return 0;
  }
  if (args.help) {
    std::cout << "Usage: modern_leveldb_performance --case ENGINE/WORKLOAD/RECORDS "
                 "--database NEW_PATH --completion-report NEW_FILE [benchmark flags]\n"
                 "Use --list-cases to list supported cases. --profile-markers requires macOS "
                 "Apple Clang. The runner owns and removes the database directory.\n";
    benchmark::PrintDefaultHelp();
    return 0;
  }
  const Case selected = FindCase(args.case_name);
  Require(!args.database.empty() && !args.completion.empty(),
          "--database and --completion-report are required");
#if !MODERN_LEVELDB_PROFILE_MARKERS
  Require(!args.profile, "profile markers require macOS Apple Clang");
#endif
  int framework_argc = static_cast<int>(args.framework.size());
  args.framework.push_back(nullptr);
  benchmark::Initialize(&framework_argc, args.framework.data());
  Require(!benchmark::ReportUnrecognizedArguments(framework_argc, args.framework.data()),
          "unrecognized benchmark argument");
  Require(benchmark::GetBenchmarkVersion() == "v1.9.5", "unexpected Google Benchmark version");
  AddContext(selected);
  if (selected.engine == "modern") {
    return RunCase<Modern>(selected, args);
  }
  return RunCase<Reference>(selected, args);
}

}  // namespace
}  // namespace modern_leveldb::profiling

int main(int argc, char** argv) {
  try {
    return modern_leveldb::profiling::Main(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "performance benchmark failed: " << error.what() << '\n';
    return 1;
  }
}
