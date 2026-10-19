#ifndef MODERN_LEVELDB_ENGINE_DEPENDENCY_H_
#define MODERN_LEVELDB_ENGINE_DEPENDENCY_H_

#include <memory>

namespace modern_leveldb {

template <typename T>
struct DependencyDeleter {
    bool owns = false;

    void operator()(T* dependency) const noexcept {
        if (owns) {
            delete dependency;
        }
    }
};

// One pointer and ownership policy, moved together. Borrowed objects must
// outlive the handle; owning handles participate in normal member unwinding.
template <typename T>
using Dependency = std::unique_ptr<T, DependencyDeleter<T>>;

template <typename T>
[[nodiscard]] Dependency<T> BorrowDependency(T& dependency) noexcept {
    return {&dependency, DependencyDeleter<T>{.owns = false}};
}

template <typename T>
[[nodiscard]] Dependency<T> OwnDependency(std::unique_ptr<T> dependency) noexcept {
    return {dependency.release(), DependencyDeleter<T>{.owns = true}};
}

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_DEPENDENCY_H_
