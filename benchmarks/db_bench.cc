#include <leveldb/db.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <locale>
#include <memory>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modern_leveldb/db.h"
#include "support/temporary_directory.h"

namespace {

struct Record {
  std::string key;
  std::string value;
};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

void Check(const modern_leveldb::Status& status) {
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
T Take(modern_leveldb::Result<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error(result.error().ToString());
  }
  return std::move(*result);
}

modern_leveldb::Options ModernOptions() {
  modern_leveldb::Options options;
  options.create_if_missing = true;
  options.write_buffer_size = 64 * 1024;
  options.block_size = 4096;
  options.compression = modern_leveldb::Compression::Snappy;
  return options;
}

class ModernDatabase final {
 public:
  explicit ModernDatabase(const std::filesystem::path& path)
      : database_(Take(modern_leveldb::Database::Open(ModernOptions(), path))) {}

  void Put(const Record& record) {
    Check(
        database_.Put(modern_leveldb::AsBytes(record.key), modern_leveldb::AsBytes(record.value)));
  }
  void Read(const Record& expected) {
    const auto value = Take(database_.Get(modern_leveldb::AsBytes(expected.key)));
    Require(value.has_value(), "Modern LevelDB lost a benchmark key");
    Require(modern_leveldb::AsStringView(*value) == expected.value,
            "Modern LevelDB value mismatch");
  }
  void Scan(const std::vector<Record>& expected) {
    auto iterator = Take(database_.NewIterator());
    Check(iterator.SeekToFirst());
    std::size_t index = 0;
    while (iterator.valid()) {
      Require(index < expected.size(), "Modern LevelDB iterator yields extra keys");
      Require(modern_leveldb::AsStringView(iterator.key()) == expected[index].key,
              "Modern LevelDB scan key mismatch");
      Require(modern_leveldb::AsStringView(iterator.value()) == expected[index].value,
              "Modern LevelDB scan value mismatch");
      ++index;
      Check(iterator.Next());
    }
    Require(index == expected.size(), "Modern LevelDB iterator lost keys");
  }

 private:
  modern_leveldb::Database database_;
};

class ReferenceDatabase final {
 public:
  explicit ReferenceDatabase(const std::filesystem::path& path) {
    leveldb::Options options;
    options.create_if_missing = true;
    options.write_buffer_size = 64 * 1024;
    options.block_size = 4096;
    options.compression = leveldb::kSnappyCompression;
    leveldb::DB* opened = nullptr;
    const auto status = leveldb::DB::Open(options, path.string(), &opened);
    database_.reset(opened);
    Check(status);
  }
  void Put(const Record& record) { Check(database_->Put({}, record.key, record.value)); }
  void Read(const Record& expected) {
    std::string value;
    leveldb::ReadOptions options;
    options.verify_checksums = true;
    Check(database_->Get(options, expected.key, &value));
    Require(value == expected.value, "LevelDB reference value mismatch");
  }
  void Scan(const std::vector<Record>& expected) {
    leveldb::ReadOptions options;
    options.verify_checksums = true;
    std::unique_ptr<leveldb::Iterator> iterator(database_->NewIterator(options));
    std::size_t index = 0;
    for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
      Require(index < expected.size(), "LevelDB reference iterator yields extra keys");
      const auto key = iterator->key();
      const auto value = iterator->value();
      Require(std::string_view(key.data(), key.size()) == expected[index].key,
              "LevelDB reference scan key mismatch");
      Require(std::string_view(value.data(), value.size()) == expected[index].value,
              "LevelDB reference scan value mismatch");
      ++index;
    }
    Check(iterator->status());
    Require(index == expected.size(), "LevelDB reference iterator lost keys");
  }

 private:
  std::unique_ptr<leveldb::DB> database_;
};

template <typename Function>
double Measure(std::size_t operations, Function function) {
  const auto start = std::chrono::steady_clock::now();
  function();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const double nanoseconds = std::chrono::duration<double, std::nano>(elapsed).count();
  Require(nanoseconds > 0, "benchmark clock did not advance");
  return nanoseconds / static_cast<double>(operations);
}

template <typename Database>
std::array<double, 3> Trial(const std::filesystem::path& path, const std::vector<Record>& records,
                            const std::vector<std::size_t>& order) {
  std::array<double, 3> result{};
  result[0] = Measure(records.size(), [&] {
    Database database(path);
    for (const auto index : order) {
      database.Put(records[index]);
    }
  });
  Database database(path);
  for (const auto index : order) {
    database.Read(records[index]);
  }
  result[1] = Measure(records.size(), [&] {
    for (const auto index : order) {
      database.Read(records[index]);
    }
  });
  constexpr std::size_t ScanPasses = 8;
  result[2] = Measure(records.size() * ScanPasses, [&] {
    for (std::size_t pass = 0; pass < ScanPasses; ++pass) {
      database.Scan(records);
    }
  });
  return result;
}

std::size_t Number(std::string_view text) {
  std::size_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  Require(parsed.ec == std::errc() && parsed.ptr == text.data() + text.size(),
          "invalid numeric benchmark argument");
  return value;
}

using Samples = std::array<std::vector<double>, 3>;

void PrintSamples(std::string_view name, const Samples& samples) {
  constexpr std::array Names{"write", "read", "scan"};
  std::cout << '"' << name << "\":{";
  for (std::size_t phase = 0; phase < Names.size(); ++phase) {
    if (phase != 0) {
      std::cout << ',';
    }
    std::cout << '"' << Names[phase] << "\":[";
    for (std::size_t trial = 0; trial < samples[phase].size(); ++trial) {
      if (trial != 0) {
        std::cout << ',';
      }
      std::cout << samples[phase][trial];
    }
    std::cout << ']';
  }
  std::cout << '}';
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::size_t entries = 4096;
    std::size_t trials = 3;
    for (int argument = 1; argument < argc; argument += 2) {
      Require(argument + 1 < argc, "benchmark options require a value");
      const std::string_view option = argv[argument];
      const std::size_t value = Number(argv[argument + 1]);
      if (option == "--entries") {
        entries = value;
      } else if (option == "--trials") {
        trials = value;
      } else {
        throw std::runtime_error("unknown benchmark option");
      }
    }
    Require(entries > 0 && entries <= 1'000'000, "entries must be in [1, 1000000]");
    Require(trials >= 3 && trials <= 31 && trials % 2 == 1, "trials must be odd and in [3, 31]");
    std::mt19937_64 random(301);
    std::vector<Record> records;
    for (std::size_t index = 0; index < entries; ++index) {
      std::string key = std::to_string(index);
      key.insert(0, 10 - key.size(), '0');
      std::string value(256, static_cast<char>('a' + index % 26));
      if (index % 4 == 0) {
        for (char& byte : value) {
          byte = static_cast<char>(random());
        }
      }
      records.push_back({std::move(key), std::move(value)});
    }
    modern_leveldb::test_support::TemporaryDirectory directory;
    Samples modern;
    Samples reference;
    std::vector<std::size_t> order(entries);
    std::iota(order.begin(), order.end(), std::size_t{0});
    for (std::size_t trial = 0; trial < trials; ++trial) {
      std::shuffle(order.begin(), order.end(), random);
      const auto modern_path = directory.path() / ("modern-" + std::to_string(trial));
      const auto reference_path = directory.path() / ("reference-" + std::to_string(trial));
      std::array<double, 3> modern_trial{};
      std::array<double, 3> reference_trial{};
      if (trial % 2 == 0) {
        modern_trial = Trial<ModernDatabase>(modern_path, records, order);
        reference_trial = Trial<ReferenceDatabase>(reference_path, records, order);
      } else {
        reference_trial = Trial<ReferenceDatabase>(reference_path, records, order);
        modern_trial = Trial<ModernDatabase>(modern_path, records, order);
      }
      for (std::size_t phase = 0; phase < modern.size(); ++phase) {
        modern[phase].push_back(modern_trial[phase]);
        reference[phase].push_back(reference_trial[phase]);
      }
    }
    std::cout.imbue(std::locale::classic());
    std::cout << std::setprecision(17) << "{\"schema_version\":1,\"entries\":" << entries
              << ",\"trials\":" << trials << ",\"samples\":{";
    PrintSamples("modern", modern);
    std::cout << ',';
    PrintSamples("leveldb", reference);
    std::cout << "}}\n";
    return std::cout.good() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "benchmark failed: " << error.what() << '\n';
    return 1;
  }
}
