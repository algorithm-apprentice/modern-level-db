#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "engine/database.h"
#include "format/write_batch.h"
#include "fuzz_support.h"
#include "support/manual_clock.h"
#include "support/manual_executor.h"
#include "support/memory_file_system.h"

namespace modern_leveldb {
namespace {

using fuzz_support::Require;
using Model = std::map<std::string, std::string>;

void FuzzDatabase(ByteView input) {
  if (input.empty() || input.size() > 4096) {
    return;
  }
  test_support::MemoryFileSystem fs;
  test_support::ManualExecutor executor;
  test_support::ManualClock clock;
  DatabaseEngineOptions options;
  options.file_system = &fs;
  options.executor = &executor;
  options.clock = &clock;
  options.create_if_missing = true;
  options.write_buffer_size = 64 * 1024;
  options.table_options.compression =
      static_cast<BlockCompression>(std::to_integer<unsigned>(input[0]) % 3);
  input = input.subspan(1);
  auto opened = DatabaseEngine::Open(options, "db");
  Require(opened.has_value());
  std::unique_ptr<DatabaseEngine> database = std::move(*opened);
  std::optional<SequenceNumber> snapshot;
  Model model;
  Model frozen;

  const auto verify_scan = [&](bool at_snapshot) {
    DatabaseEngineReadOptions read;
    if (at_snapshot && snapshot.has_value()) {
      read.snapshot = snapshot;
    }
    const Model& expected = read.snapshot.has_value() ? frozen : model;
    auto iterator = database->NewIterator(read);
    Require(iterator->SeekToFirst().has_value());
    auto entry = expected.begin();
    while (iterator->valid()) {
      Require(entry != expected.end());
      Require(AsStringView(iterator->key()) == entry->first);
      Require(AsStringView(iterator->value()) == entry->second);
      ++entry;
      Require(iterator->Next().has_value());
    }
    Require(entry == expected.end());
  };

  for (unsigned step = 0; step < 32 && input.size() >= 3; ++step) {
    const unsigned command = std::to_integer<unsigned>(input[0]);
    const unsigned key_number = std::to_integer<unsigned>(input[1]) % 16;
    const std::string key(1, static_cast<char>(key_number));
    const std::string value(((command >> 4U) % 8) * 4096,
                            static_cast<char>(std::to_integer<unsigned>(input[2])));
    input = input.subspan(3);
    switch (command % 8) {
      case 0:
      case 1: {
        EncodedWriteBatch batch;
        Require(batch.Put(AsBytes(key), AsBytes(value)).has_value());
        Require(database->Write(batch, (command & 8U) != 0).has_value());
        model[key] = value;
        break;
      }
      case 2: {
        EncodedWriteBatch batch;
        Require(batch.Delete(AsBytes(key)).has_value());
        Require(database->Write(batch, false).has_value());
        model.erase(key);
        break;
      }
      case 3: {
        const std::string other(1, static_cast<char>((key_number + 1) % 16));
        EncodedWriteBatch batch;
        Require(batch.Delete(AsBytes(other)).has_value());
        Require(batch.Put(AsBytes(key), AsBytes(value)).has_value());
        Require(database->Write(batch, true).has_value());
        model.erase(other);
        model[key] = value;
        break;
      }
      case 4: {
        DatabaseEngineReadOptions read;
        if ((command & 8U) != 0 && snapshot.has_value()) {
          read.snapshot = snapshot;
        }
        const auto actual = database->Get(AsBytes(key), read);
        Require(actual.has_value());
        const Model& expected = read.snapshot.has_value() ? frozen : model;
        const auto entry = expected.find(key);
        Require(actual->has_value() == (entry != expected.end()));
        if (entry != expected.end()) {
          Require(AsStringView(**actual) == entry->second);
        }
        break;
      }
      case 5:
        if (snapshot.has_value()) {
          database->ReleaseSnapshot(*snapshot);
        }
        snapshot = database->GetSnapshot();
        frozen = model;
        break;
      case 6:
        verify_scan((command & 8U) != 0);
        break;
      case 7:
        if (snapshot.has_value()) {
          database->ReleaseSnapshot(*snapshot);
          snapshot.reset();
        }
        executor.RunAll();
        database.reset();
        opened = DatabaseEngine::Open(options, "db");
        Require(opened.has_value());
        database = std::move(*opened);
        verify_scan(false);
        break;
    }
    executor.RunAll();
    Require(database->WaitForBackgroundWork().has_value());
  }
  verify_scan(false);
  if (snapshot.has_value()) {
    verify_scan(true);
    database->ReleaseSnapshot(*snapshot);
  }
  executor.RunAll();
}

}  // namespace
}  // namespace modern_leveldb

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  modern_leveldb::FuzzDatabase({reinterpret_cast<const std::byte*>(data), size});
  return 0;
}
