#include <filesystem>
#include <iostream>
#include <string>

#include "engine/database.h"
#include "format/write_batch.h"

int CreateFixture(const std::filesystem::path& path) {
    modern_leveldb::DatabaseEngineOptions options;
    options.create_if_missing = true;
    options.write_buffer_size = 64 * 1'024;
#if defined(_WIN32)
    options.allow_weak_namespace_durability = true;
#endif
    auto database = modern_leveldb::DatabaseEngine::Open(options, path);
    if (!database.has_value()) {
        std::cerr << database.error().ToString() << '\n';
        return 1;
    }
    modern_leveldb::EncodedWriteBatch batch;
    const std::string value(80 * 1'024, 'x');
    batch.Put(modern_leveldb::AsBytes("key"), modern_leveldb::AsBytes(value));
    const auto written = (*database)->Write(batch, true);
    if (!written.has_value()) {
        std::cerr << written.error().ToString() << '\n';
        return 1;
    }
    const auto flushed = (*database)->FlushMemTable();
    if (!flushed.has_value()) {
        std::cerr << flushed.error().ToString() << '\n';
        return 1;
    }
    const auto finished = (*database)->WaitForBackgroundWork();
    if (!finished.has_value()) {
        std::cerr << finished.error().ToString() << '\n';
        return 1;
    }
    return 0;
}

#if defined(_WIN32)
int wmain(int argc, wchar_t* argv[]) {
    return argc == 2 ? CreateFixture(std::filesystem::path(argv[1])) : 2;
}
#else
int main(int argc, char* argv[]) {
    return argc == 2 ? CreateFixture(std::filesystem::path(argv[1])) : 2;
}
#endif
