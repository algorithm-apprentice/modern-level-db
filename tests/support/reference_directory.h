#ifndef MODERN_LEVELDB_TESTS_SUPPORT_REFERENCE_DIRECTORY_H_
#define MODERN_LEVELDB_TESTS_SUPPORT_REFERENCE_DIRECTORY_H_

#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>

#include "support/temporary_directory.h"

namespace modern_leveldb::test_support {

inline std::filesystem::path SelectAsciiReferenceRoot(
    std::span<const std::filesystem::path> candidates) {
    for (const auto& candidate : candidates) {
        const auto absolute = std::filesystem::absolute(candidate);
        using UnsignedCharacter = std::make_unsigned_t<std::filesystem::path::value_type>;
        bool ascii = true;
        for (const auto character : absolute.native()) {
            if (static_cast<UnsignedCharacter>(character) > 127U) {
                ascii = false;
                break;
            }
        }
        if (ascii && std::filesystem::is_directory(absolute)) {
            return absolute;
        }
    }
    throw std::runtime_error(
        "reference fixtures require an existing writable ASCII absolute directory; "
        "set MODERN_LEVELDB_REFERENCE_TMPDIR");
}

inline std::filesystem::path ReferenceFixtureRoot() {
#if defined(_WIN32)
    wchar_t* selected = nullptr;
    std::size_t length = 0;
    if (::_wdupenv_s(&selected, &length, L"MODERN_LEVELDB_REFERENCE_TMPDIR") != 0) {
        throw std::runtime_error("could not read the reference fixture directory setting");
    }
    const std::unique_ptr<wchar_t, decltype(&std::free)> setting(selected, &std::free);
    if (setting != nullptr) {
        const std::array<std::filesystem::path, 1> candidates{std::filesystem::path(setting.get())};
        return SelectAsciiReferenceRoot(candidates);
    }
#else
    if (const char* selected = std::getenv("MODERN_LEVELDB_REFERENCE_TMPDIR");
        selected != nullptr) {
        const std::array<std::filesystem::path, 1> candidates{std::filesystem::path(selected)};
        return SelectAsciiReferenceRoot(candidates);
    }
#endif
    const std::array<std::filesystem::path, 2> candidates{std::filesystem::temp_directory_path(),
                                                          std::filesystem::current_path()};
    return SelectAsciiReferenceRoot(candidates);
}

class ReferenceTemporaryDirectory final {
public:
    ReferenceTemporaryDirectory() : directory_(ReferenceFixtureRoot()) {}
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return directory_.path(); }

private:
    TemporaryDirectory directory_;
};

}  // namespace modern_leveldb::test_support

#endif  // MODERN_LEVELDB_TESTS_SUPPORT_REFERENCE_DIRECTORY_H_
