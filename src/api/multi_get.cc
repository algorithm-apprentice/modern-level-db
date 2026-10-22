#include "api/multi_get.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "api/api_internal.h"

namespace modern_leveldb::detail {

OwningReadResult ReadBatchKey(DatabaseEngine& engine, ByteView key,
                              const DatabaseEngineReadOptions& options) {
    std::vector<std::byte> value;
    auto found = engine.Get(key, value, options);
    if (!found.has_value()) {
        return std::unexpected(std::move(found).error());
    }
    if (!*found) {
        return std::optional<std::vector<std::byte>>();
    }
    return std::optional<std::vector<std::byte>>(std::move(value));
}

std::jthread StartBatchWorker(std::function<void()> task) { return std::jthread(std::move(task)); }

namespace {

class JoinBatch final {
public:
    JoinBatch(std::vector<std::jthread>& workers, std::atomic<bool>& stopped) noexcept
        : workers_(workers), stopped_(stopped) {}
    JoinBatch(const JoinBatch&) = delete;
    JoinBatch& operator=(const JoinBatch&) = delete;
    ~JoinBatch() {
        stopped_.store(true, std::memory_order_release);
        Wait();
    }

    void Wait() {
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

private:
    std::vector<std::jthread>& workers_;
    std::atomic<bool>& stopped_;
};

struct BatchWork {
    DatabaseEngine& engine;
    std::span<const ByteView> keys;
    const DatabaseEngineReadOptions& options;
    const MultiGetRuntime& runtime;
    std::vector<OwningReadResult>& results;
    std::vector<std::exception_ptr>& exceptions;
    std::atomic<bool>& stopped;
    std::size_t concurrency;

    void Run(std::size_t worker) noexcept {
        try {
            for (std::size_t index = worker; index < keys.size(); index += concurrency) {
                if (stopped.load(std::memory_order_acquire)) {
                    return;
                }
                results[index] = runtime.read(engine, keys[index], options);
            }
        } catch (...) {
            exceptions[worker] = std::current_exception();
            stopped.store(true, std::memory_order_release);
        }
    }
};

}  // namespace

std::vector<OwningReadResult> ReadBatch(std::shared_ptr<DatabaseState> state,
                                        std::span<const ByteView> keys,
                                        DatabaseEngineReadOptions options,
                                        std::shared_ptr<SnapshotRegistration> snapshot,
                                        std::size_t max_concurrency,
                                        const MultiGetRuntime& runtime) {
    if (keys.empty()) {
        return {};
    }
    if (snapshot == nullptr) {
        snapshot = std::make_shared<SnapshotRegistration>(state);
    }
    options.snapshot = snapshot->sequence();
    std::vector<OwningReadResult> results(keys.size());
    const auto concurrency = std::min(keys.size(), max_concurrency);
    if (concurrency == 1) {
        for (std::size_t index = 0; index < keys.size(); ++index) {
            results[index] = runtime.read(state->engine(), keys[index], options);
        }
        return results;
    }

    std::vector<std::exception_ptr> exceptions(concurrency);
    std::atomic<bool> stopped = false;
    std::vector<std::jthread> workers(0);
    workers.reserve(concurrency - 1);
    BatchWork work{state->engine(), keys,       options, runtime,
                   results,         exceptions, stopped, concurrency};
    JoinBatch join(workers, stopped);
    std::exception_ptr startup;
    try {
        for (std::size_t worker = 1; worker < concurrency; ++worker) {
            auto created = runtime.start([&, worker] { work.Run(worker); });
            if (!created.joinable()) {
                throw std::logic_error("MultiGet worker factory returned a nonjoinable worker");
            }
            workers.push_back(std::move(created));
        }
    } catch (...) {
        startup = std::current_exception();
        stopped.store(true, std::memory_order_release);
    }
    if (startup == nullptr) {
        work.Run(0);
    }
    // Normal completion must not cancel partitions that have not finished yet.
    join.Wait();
    if (startup != nullptr) {
        std::rethrow_exception(startup);
    }
    for (const auto& exception : exceptions) {
        if (exception != nullptr) {
            std::rethrow_exception(exception);
        }
    }
    return results;
}

}  // namespace modern_leveldb::detail
