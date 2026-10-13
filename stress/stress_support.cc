#include "stress/stress_support.h"

#include <charconv>
#include <stdexcept>
#include <system_error>

namespace modern_leveldb::stress {
namespace {

bool StartsWithPath(const std::filesystem::path& path, const std::filesystem::path& prefix) {
    auto current = path.begin();
    auto expected = prefix.begin();
    while (current != path.end() && expected != prefix.end() && *current == *expected) {
        ++current;
        ++expected;
    }
    return expected == prefix.end();
}

std::filesystem::path AbsoluteLexicalPath(const std::filesystem::path& path,
                                          std::string_view name) {
    if (path.empty()) {
        throw std::invalid_argument(std::string(name) + " path is empty");
    }
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) {
        throw std::runtime_error("cannot resolve " + std::string(name) +
                                 " path: " + error.message());
    }
    return absolute.lexically_normal();
}

std::filesystem::path NormalizeAbsentPath(const std::filesystem::path& path,
                                          std::string_view name) {
    std::error_code error;
    if (!std::filesystem::is_directory(path.parent_path(), error)) {
        if (error && error != std::errc::no_such_file_or_directory &&
            error != std::errc::not_a_directory) {
            throw std::runtime_error("cannot inspect " + std::string(name) +
                                     " parent: " + error.message());
        }
        throw std::invalid_argument(std::string(name) + " parent directory does not exist");
    }
    const std::filesystem::path normalized = std::filesystem::weakly_canonical(path, error);
    if (error) {
        throw std::runtime_error("cannot normalize " + std::string(name) +
                                 " path: " + error.message());
    }
    if (std::filesystem::exists(normalized, error)) {
        throw std::invalid_argument(std::string(name) + " path already exists");
    }
    if (error) {
        throw std::runtime_error("cannot inspect " + std::string(name) +
                                 " path: " + error.message());
    }
    return normalized;
}

char HexDigit(unsigned value) {
    return static_cast<char>(value < 10 ? '0' + value : 'a' + (value - 10));
}

void HashByte(std::uint64_t& hash, std::uint8_t byte) {
    constexpr std::uint64_t FnvPrime = 1099511628211ULL;
    hash ^= byte;
    hash *= FnvPrime;
}

void HashUnsigned(std::uint64_t& hash, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        HashByte(hash, static_cast<std::uint8_t>(value >> shift));
    }
}

void HashBytes(std::uint64_t& hash, std::string_view bytes) {
    for (const char character : bytes) {
        HashByte(hash, static_cast<unsigned char>(character));
    }
}

std::string HashHex(std::uint64_t value) {
    constexpr std::string_view Digits = "0123456789abcdef";
    std::string result(16, '0');
    for (std::size_t index = result.size(); index > 0; --index) {
        result[index - 1] = Digits[value & 0x0fU];
        value >>= 4U;
    }
    return result;
}

}  // namespace

CheckedOutput::CheckedOutput(const std::filesystem::path& path)
    : stream_(path, std::ios::binary | std::ios::trunc) {
    if (!stream_) {
        throw std::runtime_error("cannot create stress artifact");
    }
}

void CheckedOutput::Write(std::string_view text) {
    stream_.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream_.flush();
    if (!stream_) {
        throw std::runtime_error("cannot write stress artifact");
    }
}

std::uint64_t ParseStressUnsigned(std::string_view text, std::string_view option) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::invalid_argument(std::string(option) + " requires an unsigned integer");
    }
    return value;
}

PreparedStressPaths PrepareStressPaths(const std::filesystem::path& database,
                                       const std::filesystem::path& output) {
    const std::filesystem::path lexical_database = AbsoluteLexicalPath(database, "database");
    const std::filesystem::path lexical_output = AbsoluteLexicalPath(output, "output");
    if (StartsWithPath(lexical_database, lexical_output) ||
        StartsWithPath(lexical_output, lexical_database)) {
        throw std::invalid_argument("database and output paths must be distinct and non-nested");
    }
    PreparedStressPaths result{
        NormalizeAbsentPath(lexical_database, "database"),
        NormalizeAbsentPath(lexical_output, "output"),
    };
    if (StartsWithPath(result.database, result.output) ||
        StartsWithPath(result.output, result.database)) {
        throw std::invalid_argument("database and output paths must be distinct and non-nested");
    }
    std::error_code error;
    if (!std::filesystem::create_directory(result.output, error)) {
        if (error) {
            throw std::runtime_error("cannot create stress output directory: " + error.message());
        }
        throw std::invalid_argument("stress output directory was not created");
    }
    return result;
}

std::filesystem::path PrepareStressWorkspace(const std::filesystem::path& workspace) {
    const std::filesystem::path normalized =
        NormalizeAbsentPath(AbsoluteLexicalPath(workspace, "workspace"), "workspace");
    std::error_code error;
    if (!std::filesystem::create_directory(normalized, error)) {
        if (error) {
            throw std::runtime_error("cannot create stress workspace: " + error.message());
        }
        throw std::invalid_argument("stress workspace was not created");
    }
    return normalized;
}

std::string StressCompressionName(Compression compression) {
    switch (compression) {
        case Compression::None:
            return "none";
        case Compression::Snappy:
            return "snappy";
        case Compression::Zstd:
            return "zstd";
    }
    throw std::invalid_argument("unsupported stress compression");
}

Compression ParseStressCompression(std::string_view value) {
    if (value == "none") {
        return Compression::None;
    }
    if (value == "snappy") {
        return Compression::Snappy;
    }
    if (value == "zstd") {
        return Compression::Zstd;
    }
    throw std::invalid_argument("unknown stress compression");
}

std::string StressHex(std::string_view bytes) {
    std::string encoded(bytes.size() * 2, '\0');
    std::size_t output = 0;
    for (const char character : bytes) {
        const auto byte = static_cast<unsigned char>(character);
        encoded[output++] = HexDigit(byte >> 4U);
        encoded[output++] = HexDigit(byte & 0x0fU);
    }
    return encoded;
}

std::string StableStressFingerprint(const std::map<std::string, std::string>& entries) {
    constexpr std::uint64_t FnvOffsetBasis = 14695981039346656037ULL;
    std::uint64_t hash = FnvOffsetBasis;
    for (const auto& [key, value] : entries) {
        HashUnsigned(hash, key.size());
        HashBytes(hash, key);
        HashUnsigned(hash, value.size());
        HashBytes(hash, value);
    }
    return HashHex(hash);
}

std::string SanitizeStressMessage(std::string_view message) {
    std::string result(message);
    for (char& character : result) {
        if (character == '\n' || character == '\r' || character == '\t') {
            character = ' ';
        }
    }
    return result;
}

void WriteStressText(const std::filesystem::path& path, std::string_view text) {
    CheckedOutput output(path);
    output.Write(text);
}

Options StressDatabaseOptions(Compression compression) {
    Options options;
    options.create_if_missing = true;
    options.error_if_exists = true;
    options.write_buffer_size = StressWriteBufferSize;
    options.bloom_bits_per_key = StressBloomBitsPerKey;
    options.compression = compression;
#if defined(_WIN32)
    options.allow_weak_namespace_durability = true;
#endif
    return options;
}

}  // namespace modern_leveldb::stress
