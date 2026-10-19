#ifndef MODERN_LEVELDB_ENGINE_DEPENDENCY_H_
#define MODERN_LEVELDB_ENGINE_DEPENDENCY_H_

#include <memory>
#include <utility>

namespace modern_leveldb {

// A selected service and its fixed ownership. Moves transfer the pointer;
// borrowed objects must outlive the member that refers to them.
template <typename T>
class Dependency final {
public:
    Dependency(T* pointer, bool owns) noexcept : pointer_(pointer), owns_(owns) {}
    Dependency(const Dependency&) = delete;
    Dependency& operator=(const Dependency&) = delete;
    Dependency(Dependency&& other) noexcept
        : pointer_(std::exchange(other.pointer_, nullptr)), owns_(other.owns_) {}
    Dependency& operator=(Dependency&&) = delete;

    ~Dependency() {
        static_assert(sizeof(T) > 0);
        if (owns_) {
            delete pointer_;
        }
    }

    [[nodiscard]] T* get() const noexcept { return pointer_; }

private:
    T* pointer_;
    const bool owns_;
};

template <typename T>
[[nodiscard]] Dependency<T> BorrowDependency(T& dependency) noexcept {
    return {&dependency, false};
}

template <typename T>
[[nodiscard]] Dependency<T> OwnDependency(std::unique_ptr<T> dependency) noexcept {
    return {dependency.release(), true};
}

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_DEPENDENCY_H_
