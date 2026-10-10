#include "instrumentation/read_diagnostics.h"

#if MODERN_LEVELDB_READ_DIAGNOSTICS

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace modern_leveldb::read_diagnostics {
namespace {

struct ActiveSession {
    Snapshot* snapshot = nullptr;
    std::uint64_t next_get = 0;
    bool get_active = false;
    bool sampled = false;
    BlockRole block_role = BlockRole::Other;
};

thread_local ActiveSession active;
thread_local SetupSnapshot* active_setup = nullptr;

std::size_t Index(Counter counter) noexcept { return static_cast<std::size_t>(counter); }

std::size_t Index(Stage stage) noexcept { return static_cast<std::size_t>(stage); }

std::size_t Index(FileOpenReason reason) noexcept { return static_cast<std::size_t>(reason); }

std::uint64_t Mix(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

bool ShouldSample(std::uint64_t ordinal, const Snapshot& snapshot) noexcept {
    return Mix(ordinal + snapshot.sample_seed) % snapshot.sample_denominator == 0;
}

void AddStageDuration(Stage stage, std::uint64_t nanoseconds) noexcept {
    assert(active.snapshot != nullptr && active.get_active && active.sampled);
    StageTotal& total = active.snapshot->stages[Index(stage)];
    assert(total.events < std::numeric_limits<std::uint64_t>::max());
    assert(nanoseconds <= std::numeric_limits<std::uint64_t>::max() - total.nanoseconds);
    ++total.events;
    total.nanoseconds += nanoseconds;
}

std::uint64_t ElapsedSince(std::chrono::steady_clock::time_point started) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started);
    assert(elapsed.count() >= 0);
    return static_cast<std::uint64_t>(elapsed.count());
}

void AddStage(Stage stage, std::chrono::steady_clock::time_point started) noexcept {
    AddStageDuration(stage, ElapsedSince(started));
}

}  // namespace

SetupSession::SetupSession(SetupSnapshot& snapshot) noexcept {
    assert(active_setup == nullptr);
    snapshot = {};
    active_setup = &snapshot;
}

SetupSession::~SetupSession() {
    assert(active_setup != nullptr);
    active_setup = nullptr;
}

Session::Session(Snapshot& snapshot, std::uint64_t sample_seed,
                 std::uint64_t sample_denominator) noexcept {
    assert(active.snapshot == nullptr);
    assert(sample_denominator > 0);
    snapshot = {};
    snapshot.sample_seed = sample_seed;
    snapshot.sample_denominator = sample_denominator;
    active.snapshot = &snapshot;
}

Session::~Session() {
    assert(active.snapshot != nullptr && !active.get_active);
    active = {};
}

GetScope::GetScope() noexcept {
    if (active.snapshot == nullptr) {
        return;
    }
    assert(!active.get_active);
    active_ = true;
    active.get_active = true;
    Add(Counter::Gets);
    sampled_ = ShouldSample(active.next_get++, *active.snapshot);
    active.sampled = sampled_;
    if (sampled_) {
        assert(active.snapshot->sampled_gets < std::numeric_limits<std::uint64_t>::max());
        ++active.snapshot->sampled_gets;
        started_ = std::chrono::steady_clock::now();
    }
}

GetScope::~GetScope() {
    if (!active_) {
        return;
    }
    if (sampled_) {
        AddStage(Stage::Get, started_);
    }
    active.get_active = false;
    active.sampled = false;
    active.block_role = BlockRole::Other;
}

StageScope::StageScope(Stage stage) noexcept : stage_(stage) {
    active_ = active.snapshot != nullptr && active.get_active && active.sampled;
    if (active_) {
        started_ = std::chrono::steady_clock::now();
    }
}

StageScope::~StageScope() {
    if (active_) {
        AddStage(stage_, started_);
    }
}

StageAccumulator::StageAccumulator(Stage stage) noexcept : stage_(stage) {
    active_ = active.snapshot != nullptr && active.get_active && active.sampled;
}

StageAccumulator::~StageAccumulator() {
    if (!active_) {
        return;
    }
    if (running_) {
        Pause();
    }
    AddStageDuration(stage_, nanoseconds_);
}

void StageAccumulator::Resume() noexcept {
    if (!active_) {
        return;
    }
    assert(!running_);
    running_ = true;
    started_ = std::chrono::steady_clock::now();
}

void StageAccumulator::Pause() noexcept {
    if (!active_) {
        return;
    }
    assert(running_);
    const std::uint64_t elapsed = ElapsedSince(started_);
    assert(elapsed <= std::numeric_limits<std::uint64_t>::max() - nanoseconds_);
    nanoseconds_ += elapsed;
    running_ = false;
}

BlockRoleScope::BlockRoleScope(BlockRole role) noexcept {
    active_ = active.snapshot != nullptr && active.get_active;
    if (active_) {
        previous_ = active.block_role;
        active.block_role = role;
    }
}

BlockRoleScope::~BlockRoleScope() {
    if (active_) {
        active.block_role = previous_;
    }
}

void Add(Counter counter, std::uint64_t amount) noexcept {
    if (active.snapshot == nullptr || !active.get_active) {
        return;
    }
    std::uint64_t& value = active.snapshot->counters[Index(counter)];
    assert(amount <= std::numeric_limits<std::uint64_t>::max() - value);
    value += amount;
}

void RecordDecodedEntry(bool restart) noexcept {
    if (active.snapshot == nullptr || !active.get_active) {
        return;
    }
    if (active.block_role == BlockRole::Index) {
        if (restart) {
            Add(Counter::RestartEntriesDecoded);
        }
        Add(Counter::IndexEntriesDecoded);
    } else if (active.block_role == BlockRole::Data) {
        if (restart) {
            Add(Counter::RestartEntriesDecoded);
        }
        Add(Counter::DataEntriesDecoded);
    }
}

void RecordFileOpen(FileOpenReason reason, std::uint64_t bytes) noexcept {
    if (active_setup == nullptr) {
        return;
    }
    FileOpenTotal& total = active_setup->file_opens[Index(reason)];
    assert(total.files < std::numeric_limits<std::uint64_t>::max());
    assert(bytes <= std::numeric_limits<std::uint64_t>::max() - total.bytes);
    ++total.files;
    total.bytes += bytes;
}

}  // namespace modern_leveldb::read_diagnostics

#endif
