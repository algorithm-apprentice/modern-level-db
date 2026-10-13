#ifndef MODERN_LEVELDB_TESTS_SUPPORT_TEXT_FILE_H_
#define MODERN_LEVELDB_TESTS_SUPPORT_TEXT_FILE_H_

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace modern_leveldb::test_support {

inline std::string ReadTextFile(const std::filesystem::path& path) {
    std::error_code error;
    const std::uintmax_t file_size = std::filesystem::file_size(path, error);
    if (error || file_size > std::numeric_limits<std::size_t>::max() ||
        file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("cannot size test text file");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read test text file");
    }
    std::string result(static_cast<std::size_t>(file_size), '\0');
    input.read(result.data(), static_cast<std::streamsize>(result.size()));
    if (input.gcount() != static_cast<std::streamsize>(result.size())) {
        throw std::runtime_error("cannot read complete test text file");
    }
    return result;
}

}  // namespace modern_leveldb::test_support

#endif  // MODERN_LEVELDB_TESTS_SUPPORT_TEXT_FILE_H_
