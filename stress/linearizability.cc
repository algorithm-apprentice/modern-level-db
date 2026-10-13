#include "stress/linearizability.h"

#include <set>
#include <stdexcept>
#include <utility>

namespace modern_leveldb::stress {
namespace {

void ValidateOperation(const RegisterOperation& operation) {
    if (operation.invoke >= operation.complete) {
        throw std::invalid_argument("register operation interval is not increasing");
    }
    switch (operation.kind) {
        case RegisterOperationKind::Store:
            if (!operation.value.has_value() || operation.observed.has_value()) {
                throw std::invalid_argument("store operation fields are invalid");
            }
            break;
        case RegisterOperationKind::Delete:
            if (operation.value.has_value() || operation.observed.has_value()) {
                throw std::invalid_argument("delete operation fields are invalid");
            }
            break;
        case RegisterOperationKind::Read:
            if (operation.value.has_value()) {
                throw std::invalid_argument("read operation fields are invalid");
            }
            break;
    }
}

std::vector<std::size_t> PredecessorMasks(std::span<const RegisterOperation> operations) {
    std::vector<std::size_t> result(operations.size());
    for (std::size_t selected = 0; selected != operations.size(); ++selected) {
        for (std::size_t candidate = 0; candidate != operations.size(); ++candidate) {
            if (operations[candidate].complete < operations[selected].invoke) {
                result[selected] |= std::size_t{1} << candidate;
            }
        }
    }
    return result;
}

bool Search(std::optional<std::string> state, std::span<const RegisterOperation> operations,
            std::span<const std::size_t> predecessors, std::size_t used,
            std::vector<std::size_t>& order, LinearizationWitness& witness) {
    if (order.size() == operations.size()) {
        witness.operation_ids = order;
        witness.final_value = std::move(state);
        return true;
    }
    for (std::size_t index = 0; index != operations.size(); ++index) {
        const std::size_t bit = std::size_t{1} << index;
        if ((used & bit) != 0 || (predecessors[index] & ~used) != 0) {
            continue;
        }
        std::optional<std::string> next = state;
        const RegisterOperation& operation = operations[index];
        switch (operation.kind) {
            case RegisterOperationKind::Store:
                next = operation.value;
                break;
            case RegisterOperationKind::Delete:
                next.reset();
                break;
            case RegisterOperationKind::Read:
                if (operation.observed != state) {
                    continue;
                }
                break;
        }
        order.push_back(operation.id);
        if (Search(std::move(next), operations, predecessors, used | bit, order, witness)) {
            return true;
        }
        order.pop_back();
    }
    return false;
}

}  // namespace

std::optional<LinearizationWitness> FindRegisterLinearization(
    std::optional<std::string> initial_value, std::span<const RegisterOperation> operations) {
    if (operations.size() > MaxRegisterOperations) {
        throw std::invalid_argument("register history exceeds the bounded checker");
    }
    std::set<std::size_t> ids;
    std::set<std::uint64_t> ticks;
    for (const RegisterOperation& operation : operations) {
        ValidateOperation(operation);
        if (!ids.insert(operation.id).second) {
            throw std::invalid_argument("register history has duplicate operation IDs");
        }
        if (!ticks.insert(operation.invoke).second || !ticks.insert(operation.complete).second) {
            throw std::invalid_argument("register history has duplicate event ticks");
        }
    }
    const std::vector<std::size_t> predecessors = PredecessorMasks(operations);
    std::vector<std::size_t> order;
    order.reserve(operations.size());
    LinearizationWitness witness;
    if (!Search(std::move(initial_value), operations, predecessors, 0, order, witness)) {
        return std::nullopt;
    }
    return witness;
}

}  // namespace modern_leveldb::stress
