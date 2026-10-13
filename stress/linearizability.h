#ifndef MODERN_LEVELDB_STRESS_LINEARIZABILITY_H_
#define MODERN_LEVELDB_STRESS_LINEARIZABILITY_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace modern_leveldb::stress {

inline constexpr std::size_t MaxRegisterOperations = 9;

enum class RegisterOperationKind : std::uint8_t {
    Store,
    Delete,
    Read,
};

struct RegisterOperation {
    std::size_t id = 0;
    RegisterOperationKind kind = RegisterOperationKind::Read;
    std::optional<std::string> value;
    std::optional<std::string> observed;
    std::uint64_t invoke = 0;
    std::uint64_t complete = 0;

    friend bool operator==(const RegisterOperation&, const RegisterOperation&) = default;
};

struct LinearizationWitness {
    std::vector<std::size_t> operation_ids;
    std::optional<std::string> final_value;

    friend bool operator==(const LinearizationWitness&, const LinearizationWitness&) = default;
};

[[nodiscard]] std::optional<LinearizationWitness> FindRegisterLinearization(
    std::optional<std::string> initial_value, std::span<const RegisterOperation> operations);

}  // namespace modern_leveldb::stress

#endif  // MODERN_LEVELDB_STRESS_LINEARIZABILITY_H_
