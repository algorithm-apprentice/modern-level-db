#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "base/result_internal.h"
#include "database_test_support.h"
#include "engine/database.h"
#include "metadata/version_set.h"
#include "support/manual_executor.h"
#include "support/memory_file_system.h"

namespace {

constexpr std::size_t NoAllocationFailure = std::numeric_limits<std::size_t>::max();
thread_local std::size_t allocation_failure_threshold = NoAllocationFailure;

void* Allocate(std::size_t size) {
    if (size >= allocation_failure_threshold) {
        throw std::bad_alloc();
    }
    void* pointer = std::malloc(size == 0 ? 1 : size);
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    return pointer;
}

}  // namespace

void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace modern_leveldb {
namespace {

enum class FileKind { Log, Table, Manifest, Other };
enum class FaultMode {
    ThrowAppend,
    ReturnAppendError,
    ThrowClose,
    ExhaustAfterFlush,
    ExhaustAfterSync,
    ExhaustAfterNamespaceSync
};

struct Fault {
    FileKind target = FileKind::Log;
    FaultMode mode = FaultMode::ThrowAppend;
    bool armed = false;
    bool fired = false;
    std::size_t partial_bytes = 0;
    std::size_t namespace_barriers_before_fault = 2;
};

class FaultFile final : public WritableFile {
public:
    FaultFile(std::unique_ptr<WritableFile> inner, FileKind kind, Fault& fault)
        : inner_(std::move(inner)),
          kind_(kind),
          fault_(fault),
          failure_(std::unexpected(Error::Io(
              "injected partial storage write with an owning diagnostic allocation failure"))) {}

    Status Append(ByteView data) override {
        if (Eligible(FaultMode::ThrowAppend) || Eligible(FaultMode::ReturnAppendError)) {
            fault_.armed = false;
            fault_.fired = true;
            const std::size_t size = data.empty() ? 0 : 1;
            const Status prefix = inner_->Append(data.first(size));
            if (!prefix.has_value()) {
                return prefix;
            }
            fault_.partial_bytes += size;
            if (fault_.mode == FaultMode::ReturnAppendError) {
                // Leave small Debug-library iterator bookkeeping available.
                allocation_failure_threshold = 32;
                return std::move(failure_);
            }
            allocation_failure_threshold = 0;
            throw std::bad_alloc();
        }
        return inner_->Append(data);
    }

    Status Flush() override {
        const Status flushed = inner_->Flush();
        if (flushed.has_value() && Eligible(FaultMode::ExhaustAfterFlush)) {
            fault_.armed = false;
            fault_.fired = true;
            allocation_failure_threshold = 0;
        }
        return flushed;
    }
    Status Sync() override {
        const Status synced = inner_->Sync();
        if (synced.has_value() && Eligible(FaultMode::ExhaustAfterSync)) {
            fault_.armed = false;
            fault_.fired = true;
            allocation_failure_threshold = 0;
        }
        return synced;
    }
    Status Close() override {
        Status closed = inner_->Close();
        if (Eligible(FaultMode::ThrowClose)) {
            fault_.armed = false;
            fault_.fired = true;
            allocation_failure_threshold = 0;
            throw std::bad_alloc();
        }
        return closed;
    }

private:
    bool Eligible(FaultMode mode) const noexcept {
        return fault_.armed && fault_.target == kind_ && fault_.mode == mode;
    }

    std::unique_ptr<WritableFile> inner_;
    FileKind kind_;
    Fault& fault_;
    Status failure_;
};

class FaultFileSystem final : public test_support::MemoryFileSystem {
public:
    explicit FaultFileSystem(Fault& fault) : fault_(fault) {}

    Result<std::unique_ptr<WritableFile>> OpenWritable(const std::filesystem::path& path) override {
        auto opened = MemoryFileSystem::OpenWritable(path);
        if (!opened.has_value()) {
            return std::unexpected(std::move(opened).error());
        }
        FileKind kind = FileKind::Other;
        if (path.extension() == ".log") {
            kind = FileKind::Log;
        } else if (path.extension() == ".ldb") {
            kind = FileKind::Table;
        } else if (path.filename().string().starts_with("MANIFEST-")) {
            kind = FileKind::Manifest;
        }
        return std::make_unique<FaultFile>(std::move(*opened), kind, fault_);
    }

    Status SyncDirectory(const std::filesystem::path& path) override {
        const Status synced = MemoryFileSystem::SyncDirectory(path);
        if (synced.has_value() && fault_.armed &&
            fault_.mode == FaultMode::ExhaustAfterNamespaceSync &&
            --fault_.namespace_barriers_before_fault == 0) {
            fault_.armed = false;
            fault_.fired = true;
            allocation_failure_threshold = 0;
        }
        return synced;
    }

private:
    Fault& fault_;
};

class FaultExecutor final : public BackgroundExecutor {
public:
    Status Schedule(BackgroundTask task) override {
        if (exhaust_on_schedule) {
            allocation_failure_threshold = 0;
            throw std::bad_alloc();
        }
        return queued.Schedule(std::move(task));
    }

    test_support::ManualExecutor queued;
    bool exhaust_on_schedule = false;
};

void RequireOrExit(bool condition, const char* message) {
    if (!condition) {
        std::fputs(message, stderr);
        // A violated invariant may leave exhaustion armed or shutdown unable to progress.
        std::_Exit(1);
    }
}

struct AllocationReset {
    ~AllocationReset() { allocation_failure_threshold = NoAllocationFailure; }
};

template <typename Operation>
bool RejectedOrThrew(Operation&& operation) {
    try {
        AllocationReset reset;
        return !operation().has_value();
    } catch (const std::bad_alloc&) {
        return true;
    }
}

std::unique_ptr<DatabaseEngine> Open(FaultFileSystem& files, FaultExecutor& executor) {
    DatabaseEngineOptions options;
    options.create_if_missing = true;
    options.file_system = &files;
    options.executor = &executor;
    options.memtable_size_threshold = 64U * 1024U;
    auto opened = DatabaseEngine::Open(options, "fault-db");
    RequireOrExit(opened.has_value(), "setup open failed");
    return std::move(*opened);
}

Status Put(DatabaseEngine& database, std::string_view key, std::string_view value) {
    EncodedWriteBatch batch;
    batch.Put(AsBytes(key), AsBytes(value));
    return database.Write(batch, false);
}

void RequireTerminal(DatabaseEngine& database, FaultFileSystem& files) {
    const auto state = database.GetState();
    RequireOrExit(state.has_value() && state->sticky_error.has_value(),
                  "uncertain operation did not latch a terminal error");
    RequireOrExit(state->write_queue_depth == 0, "a stack writer remained queued");
    RequireOrExit(!state->background_work_scheduled, "accepted background state was stranded");
    const std::size_t operations = files.operations().size();
    RequireOrExit(!Put(database, "later", "must be rejected").has_value(),
                  "a later write was acknowledged after uncertainty");
    RequireOrExit(files.operations().size() == operations,
                  "a rejected later write performed storage I/O");
}

[[noreturn]] void ForegroundFailure(FaultMode mode) {
    Fault fault;
    fault.mode = mode;
    FaultFileSystem files(fault);
    FaultExecutor executor;
    auto database = Open(files, executor);
    RequireOrExit(Put(*database, "seed", "acknowledged").has_value(), "seed failed");
    EncodedWriteBatch batch;
    const std::string value(mode == FaultMode::ExhaustAfterFlush ? 128U * 1024U : 32U, 'v');
    batch.Put(AsBytes("uncertain"), AsBytes(value));
    batch.SetSequence(77);
    fault.armed = true;
    RequireOrExit(RejectedOrThrew([&] { return database->Write(batch, false); }),
                  "uncertain write returned success");
    RequireOrExit(fault.fired, "fault was not reached");
    if (mode != FaultMode::ExhaustAfterFlush) {
        RequireOrExit(fault.partial_bytes == 1, "partial side effect was not established");
    }
    RequireTerminal(*database, files);
    const auto state = database->GetState();
    RequireOrExit(state->last_sequence == 1, "failed batch became visible");
    RequireOrExit(batch.sequence() == 77, "failed commit leaked its reserved sequence");
    database.reset();
    std::exit(0);
}

[[noreturn]] void RotationFailure(bool scheduling) {
    Fault fault;
    fault.mode = FaultMode::ThrowClose;
    FaultFileSystem files(fault);
    FaultExecutor executor;
    auto database = Open(files, executor);
    const std::string large(64U * 1024U, 'v');
    RequireOrExit(Put(*database, "large", large).has_value(), "fill failed");
    EncodedWriteBatch next;
    next.Put(AsBytes("rotate"), AsBytes("value"));
    fault.armed = !scheduling;
    executor.exhaust_on_schedule = scheduling;
    RequireOrExit(RejectedOrThrew([&] { return database->Write(next, false); }),
                  "failed rotation returned success");
    RequireTerminal(*database, files);
    RequireOrExit(executor.queued.queued() == 0, "a rejected scheduler accepted a task");
    database.reset();
    std::exit(0);
}

[[noreturn]] void BackgroundFailure(FileKind target) {
    Fault fault;
    fault.target = target;
    FaultFileSystem files(fault);
    FaultExecutor executor;
    auto database = Open(files, executor);
    const std::string large(64U * 1024U, 'v');
    RequireOrExit(Put(*database, "large", large).has_value(), "fill failed");
    RequireOrExit(Put(*database, "next", "acknowledged").has_value(), "rotation failed");
    RequireOrExit(executor.queued.queued() == 1, "flush was not admitted");
    fault.armed = true;
    try {
        AllocationReset reset;
        static_cast<void>(executor.queued.RunOne());
    } catch (const std::bad_alloc&) {
    }
    RequireOrExit(fault.fired && fault.partial_bytes == 1, "background fault was not reached");
    RequireTerminal(*database, files);
    RequireOrExit(executor.queued.queued() == 0, "failed work was rescheduled");
    database.reset();
    std::exit(0);
}

[[noreturn]] void MetadataFailure(bool locked, bool staging,
                                  FaultMode mode = FaultMode::ThrowAppend) {
    Fault fault;
    fault.target = FileKind::Manifest;
    fault.mode = mode;
    FaultFileSystem files(fault);
    files.AddDirectory("metadata-db");
    InternalKeyComparator comparator(BytewiseComparator());
    auto opened = VersionSet::Create(files, "metadata-db", comparator);
    RequireOrExit(opened.has_value(), "metadata setup failed");
    auto versions = std::move(*opened);
    RequireOrExit(versions->LogAndApply(VersionEdit{}).has_value(), "initial edit failed");
    const auto previous = versions->current();
    std::mutex mutex;
    std::unique_lock lock(mutex);
    VersionEdit edit;
    fault.armed = !staging;
    if (staging) {
        allocation_failure_threshold = sizeof(std::array<std::optional<InternalKey>, NumLevels>);
    }
    RequireOrExit(RejectedOrThrew([&] {
                      return locked ? versions->LogAndApply(edit, lock)
                                    : versions->LogAndApply(edit);
                  }),
                  "failed metadata operation returned success");
    RequireOrExit(lock.owns_lock(), "metadata exception lost the database lock");
    RequireOrExit(versions->current() == previous, "failed metadata operation published a Version");
    const std::size_t operations = files.operations().size();
    if (staging) {
        RequireOrExit(!fault.fired, "staging failure reached persistence");
        RequireOrExit(versions->LogAndApply(VersionEdit{}).has_value(),
                      "safe staging allocation failure poisoned metadata");
    } else {
        RequireOrExit(fault.fired && fault.partial_bytes == 1, "metadata side effect missing");
        RequireOrExit(!versions->LogAndApply(VersionEdit{}).has_value(),
                      "metadata owner accepted another edit after uncertain persistence");
        RequireOrExit(files.operations().size() == operations,
                      "rejected metadata edit performed I/O");
    }
    versions.reset();
    std::exit(0);
}

[[noreturn]] void TerminalDiagnosticFailure(bool exhausted, bool exception) {
    constexpr std::string_view ExceptionContext =
        "first exception with enough owning context to require a diagnostic allocation";
    TerminalError failure;
    const Error first = Error::Io(std::string(256, 'x'));
    if (exhausted) {
        allocation_failure_threshold = 0;
    }
    if (exception) {
        failure.RecordException(ExceptionContext);
    } else {
        failure.Record(first);
    }
    RequireOrExit(failure.has_value(), "diagnostic allocation lost the terminal signal");
    const Error* retained = &*failure;
    allocation_failure_threshold = 0;
    failure.Record(first);
    failure.RecordException("later exception");
    bool observer_threw = false;
    try {
        static_cast<void>(failure.Snapshot());
    } catch (const std::bad_alloc&) {
        observer_threw = true;
    }
    allocation_failure_threshold = NoAllocationFailure;
    RequireOrExit(observer_threw, "observer allocation failure was not reached");
    RequireOrExit(failure.has_value() && &*failure == retained,
                  "a failed observer or repeated failure changed terminal ownership");
    const auto snapshot = failure.Snapshot();
    RequireOrExit(snapshot.has_value(), "a later observer lost the retained failure");
    RequireOrExit(snapshot->code() == (exhausted || exception ? ErrorCode::Aborted : ErrorCode::Io),
                  "the first error code changed");
    if (!exhausted) {
        RequireOrExit(snapshot->message() == (exception ? ExceptionContext : first.message()),
                      "the first detailed error was overwritten");
    }
    std::exit(0);
}

[[noreturn]] void MetadataPublication(bool locked) {
    Fault fault;
    fault.target = FileKind::Manifest;
    fault.mode = FaultMode::ExhaustAfterSync;
    FaultFileSystem files(fault);
    files.AddDirectory("metadata-db");
    InternalKeyComparator comparator(BytewiseComparator());
    auto opened = VersionSet::Create(files, "metadata-db", comparator);
    RequireOrExit(opened.has_value(), "metadata setup failed");
    auto versions = std::move(*opened);
    const auto previous = versions->current();
    std::optional<VersionSet::ReadPin> pinned(versions->PinCurrent());
    const auto number = versions->NewFileNumber();
    const auto key = InternalKey::Create(AsBytes("a"), 1, ValueKind::Value).value();
    VersionEdit edit;
    RequireOrExit(
        edit.AddFile(0, {.number = number, .file_size = 100, .smallest = key, .largest = key})
            .has_value(),
        "metadata edit setup failed");
    RequireOrExit(edit.AddCompactPointer(0, key).has_value(), "pointer setup failed");
    std::mutex mutex;
    std::unique_lock lock(mutex);
    fault.armed = true;
    bool published = false;
    bool threw = false;
    try {
        AllocationReset reset;
        published =
            (locked ? versions->LogAndApply(edit, lock) : versions->LogAndApply(edit)).has_value();
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RequireOrExit(fault.fired, "post-persistence exhaustion was not reached");
    RequireOrExit(!threw && published, "durable publication allocated or failed");
    RequireOrExit(lock.owns_lock(), "publication did not restore the lock");
    RequireOrExit(versions->current() != previous, "durable Version was not published");
    RequireOrExit(versions->current()->files(0).size() == 1 &&
                      versions->current()->files(0).front()->number == number,
                  "published topology differs from the durable edit");
    RequireOrExit(pinned->value().files(0).empty(), "publication changed a pinned old Version");
    RequireOrExit(std::ranges::equal(versions->compact_pointers()[0]->encoded(), key.encoded()),
                  "durable compact pointer was not published");
    pinned.reset();
    versions.reset();
    std::exit(0);
}

[[noreturn]] void PreparedPointerCopyFailure() {
    Fault fault;
    FaultFileSystem files(fault);
    files.AddDirectory("metadata-db");
    InternalKeyComparator comparator(BytewiseComparator());
    auto opened = VersionSet::Create(files, "metadata-db", comparator);
    RequireOrExit(opened.has_value(), "metadata setup failed");
    auto versions = std::move(*opened);
    const std::string user_key(512, 'k');
    const auto key = InternalKey::Create(AsBytes(user_key), 1, ValueKind::Value).value();
    VersionEdit seed;
    RequireOrExit(seed.AddCompactPointer(2, key).has_value(), "pointer setup failed");
    RequireOrExit(versions->LogAndApply(seed).has_value(), "seed metadata failed");
    const auto previous = versions->current();
    const std::size_t operations = files.operations().size();
    allocation_failure_threshold = key.encoded().size();

    RequireOrExit(RejectedOrThrew([&] { return versions->LogAndApply(VersionEdit{}); }),
                  "prepared compact-pointer copy did not reach allocation failure");
    RequireOrExit(files.operations().size() == operations && versions->current() == previous,
                  "a staging-copy failure reached persistence or changed topology");
    RequireOrExit(std::ranges::equal(versions->compact_pointers()[2]->encoded(), key.encoded()),
                  "a staging-copy failure changed the current pointer");
    RequireOrExit(versions->LogAndApply(VersionEdit{}).has_value(),
                  "a safe staging-copy failure poisoned the owner");
    versions.reset();
    std::exit(0);
}

[[noreturn]] void LatePinnedPublication(bool recovered) {
    Fault fault;
    fault.target = FileKind::Manifest;
    fault.mode = recovered ? FaultMode::ExhaustAfterNamespaceSync : FaultMode::ExhaustAfterSync;
    FaultFileSystem files(fault);
    files.AddDirectory("metadata-db");
    InternalKeyComparator comparator(BytewiseComparator());
    auto opened = VersionSet::Create(files, "metadata-db", comparator);
    RequireOrExit(opened.has_value(), "metadata setup failed");
    auto versions = std::move(*opened);
    const auto old_key = InternalKey::Create(AsBytes("old"), 1, ValueKind::Value).value();
    VersionEdit seed;
    RequireOrExit(seed.AddCompactPointer(2, old_key).has_value(), "old pointer setup failed");
    RequireOrExit(versions->LogAndApply(seed).has_value(), "seed metadata failed");
    if (recovered) {
        versions.reset();
        opened = VersionSet::Recover(files, "metadata-db", comparator);
        RequireOrExit(opened.has_value(), "metadata recovery setup failed");
        versions = std::move(*opened);
    }
    const std::weak_ptr<const Version> previous = versions->current();
    const auto number = versions->NewFileNumber();
    const auto key = InternalKey::Create(AsBytes("a"), 7, ValueKind::Value).value();
    VersionEdit edit;
    RequireOrExit(
        edit.AddFile(0, {.number = number, .file_size = 100, .smallest = key, .largest = key})
            .has_value(),
        "metadata edit setup failed");
    RequireOrExit(edit.AddCompactPointer(0, key).has_value(), "new pointer setup failed");
    RequireOrExit(edit.AddCompactPointer(2, key).has_value(), "replacement pointer setup failed");
    versions->SetLastSequence(7);
    database_test::Gate gate;
    gate.Close([](std::string_view) { return true; });
    std::size_t namespace_barriers = 0;
    files.SetOperationHook([&](std::string_view operation) {
        const bool final_barrier =
            recovered ? operation == "sync_directory metadata-db" && ++namespace_barriers == 2
                      : operation.starts_with("sync MANIFEST-");
        return final_barrier ? gate(operation) : Status{};
    });
    std::mutex mutex;
    std::optional<VersionSet::ReadPin> pinned;
    bool published = false;
    bool returned_locked = false;
    bool threw = false;
    fault.armed = true;
    std::thread publisher([&] {
        try {
            AllocationReset reset;
            std::unique_lock lock(mutex);
            published = versions->LogAndApply(edit, lock).has_value();
            returned_locked = lock.owns_lock();
        } catch (const std::bad_alloc&) {
            threw = true;
        }
    });
    gate.WaitUntilReached();
    std::uint64_t advanced_number = 0;
    {
        const std::unique_lock lock(mutex);
        pinned.emplace(versions->PinCurrent());
        advanced_number = versions->NewFileNumber();
        versions->SetLastSequence(8);
    }
    gate.Open();
    publisher.join();
    files.SetOperationHook({});
    RequireOrExit(fault.fired && !threw && published && returned_locked,
                  "post-barrier publication failed or lost the lock");
    RequireOrExit(
        versions->last_sequence() == 8 && versions->NewFileNumber() == advanced_number + 1,
        "publication rolled back foreground counter advances");
    RequireOrExit(!previous.expired() && pinned->value().files(0).empty(),
                  "a late raw pin lost its old Version");
    {
        const std::unique_lock lock(mutex);
        pinned.reset();
    }
    RequireOrExit(previous.expired(), "the released late pin retained its old Version");
    versions.reset();
    opened = VersionSet::Recover(files, "metadata-db", comparator);
    RequireOrExit(opened.has_value(), "published metadata could not be recovered");
    RequireOrExit((*opened)->current()->files(0).size() == 1 &&
                      (*opened)->current()->files(0).front()->number == number,
                  "recovery lost the durably published topology");
    RequireOrExit(
        std::ranges::equal((*opened)->compact_pointers()[0]->encoded(), key.encoded()) &&
            std::ranges::equal((*opened)->compact_pointers()[2]->encoded(), key.encoded()),
        "new-MANIFEST snapshot and edit did not preserve compact pointers");
    opened->reset();
    std::exit(0);
}

[[noreturn]] void FallbackConstructionFailure() {
    bool threw = false;
    try {
        AllocationReset reset;
        allocation_failure_threshold = 0;
        TerminalError failure;
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RequireOrExit(threw, "owner fallback construction did not report allocation failure");
    std::exit(0);
}

void WaitForWriters(DatabaseEngine& database, std::size_t count) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto state = database.GetState();
        RequireOrExit(state.has_value(), "queue observation failed");
        if (state->write_queue_depth == count) {
            return;
        }
        std::this_thread::yield();
    }
    RequireOrExit(false, "writers did not reach the required queue state");
}

[[noreturn]] void GroupedFailure() {
    Fault fault;
    FaultFileSystem files(fault);
    FaultExecutor executor;
    auto database = Open(files, executor);
    database_test::Gate gate;
    gate.Close([](std::string_view operation) {
        return operation.starts_with("flush ") && operation.ends_with(".log");
    });
    files.SetOperationHook(std::ref(gate));
    std::array<EncodedWriteBatch, 3> batches;
    for (std::size_t index = 0; index < batches.size(); ++index) {
        batches[index].Put(AsBytes(std::to_string(index)), AsBytes("value"));
        batches[index].SetSequence(77);
    }
    enum class Outcome { Unset, Success, GroupAborted, OtherFailure, Threw };
    std::array<Outcome, 3> outcomes{};
    auto write = [&](std::size_t index) {
        try {
            AllocationReset reset;
            const Status status = database->Write(batches[index], false);
            if (status.has_value()) {
                outcomes[index] = Outcome::Success;
            } else if (status.error().message() ==
                       "write aborted by an exception while its group committed") {
                outcomes[index] = Outcome::GroupAborted;
            } else {
                outcomes[index] = Outcome::OtherFailure;
            }
        } catch (const std::bad_alloc&) {
            outcomes[index] = Outcome::Threw;
        }
    };
    std::thread first(write, 0);
    gate.WaitUntilReached();
    std::thread leader(write, 1);
    WaitForWriters(*database, 2);
    std::thread follower(write, 2);
    WaitForWriters(*database, 3);
    fault.armed = true;
    gate.Open();
    first.join();
    leader.join();
    follower.join();
    files.SetOperationHook({});
    RequireOrExit(outcomes[0] == Outcome::Success, "the preceding writer did not complete");
    RequireOrExit(outcomes[1] == Outcome::Threw, "the group leader did not reach exhaustion");
    RequireOrExit(outcomes[2] == Outcome::GroupAborted,
                  "the admitted follower did not receive the preallocated group result");
    RequireOrExit(fault.fired && fault.partial_bytes == 1, "group partial write missing");
    for (const auto& batch : batches) {
        RequireOrExit(batch.sequence() == 77, "group completion changed a caller batch");
    }
    RequireTerminal(*database, files);
    RequireOrExit(database->GetState()->last_sequence == 1, "the failed group was published");
    database.reset();
    std::exit(0);
}

[[noreturn]] void ReschedulingFailure() {
    Fault fault;
    FaultFileSystem files(fault);
    FaultExecutor executor;
    auto database = Open(files, executor);
    const std::string large(64U * 1024U, 'v');
    RequireOrExit(Put(*database, "a", large).has_value(), "first fill failed");
    RequireOrExit(Put(*database, "b", "value").has_value(), "first rotation failed");
    RequireOrExit(Put(*database, "c", large).has_value(), "second fill failed");
    database_test::Gate gate;
    gate.Close([](std::string_view operation) {
        return operation.starts_with("remove ") && operation.ends_with(".log");
    });
    files.SetOperationHook(std::ref(gate));
    bool escaped = false;
    std::thread runner([&] {
        try {
            AllocationReset reset;
            static_cast<void>(executor.queued.RunOne());
        } catch (const std::bad_alloc&) {
            escaped = true;
        }
    });
    gate.WaitUntilReached();
    RequireOrExit(Put(*database, "d", "acknowledged").has_value(), "second rotation failed");
    executor.exhaust_on_schedule = true;
    gate.Open();
    runner.join();
    files.SetOperationHook({});
    RequireOrExit(!escaped, "rescheduling exhaustion escaped accepted-task bookkeeping");
    RequireTerminal(*database, files);
    RequireOrExit(executor.queued.queued() == 0, "failed epilogue rescheduled work");
    const auto state = database->GetState();
    RequireOrExit(state->immutable_memtable_bytes.has_value(),
                  "the unscheduled immutable source was discarded");
    RequireOrExit(!database->WaitForBackgroundWork().has_value(), "wait lost the terminal error");
    database.reset();
    std::exit(0);
}

TEST(WriteFailureDeathTest, PartialWalExceptionLatchesBeforeAllocatingDiagnostics) {
    EXPECT_EXIT(ForegroundFailure(FaultMode::ThrowAppend), ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, ReturnedWalFailureCannotLoseItsLatchDuringDiagnosticCopy) {
    EXPECT_EXIT(ForegroundFailure(FaultMode::ReturnAppendError), ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, MemtableAllocationFailureAfterCompleteWalRemainsTerminal) {
    EXPECT_EXIT(ForegroundFailure(FaultMode::ExhaustAfterFlush), ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, RotationCloseExceptionRemainsTerminalUnderExhaustion) {
    EXPECT_EXIT(RotationFailure(false), ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, SchedulingExceptionDoesNotStrandAcceptedWorkState) {
    EXPECT_EXIT(RotationFailure(true), ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, BackgroundTableExceptionSettlesTaskStateUnderExhaustion) {
    EXPECT_EXIT(BackgroundFailure(FileKind::Table), ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, BackgroundManifestExceptionSettlesTaskStateUnderExhaustion) {
    EXPECT_EXIT(BackgroundFailure(FileKind::Manifest), ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, StandaloneMetadataOwnerRejectsEditsAfterPersistenceException) {
    EXPECT_EXIT(MetadataFailure(false, false), ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, LockAwareMetadataOwnerRestoresLockAndRejectsLaterEdits) {
    EXPECT_EXIT(MetadataFailure(true, false), ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, GenuinePrePersistenceAllocationFailureCanBeRetried) {
    EXPECT_EXIT(MetadataFailure(true, true), ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, ReturnedMetadataFailureLatchesBeforeDiagnosticCopy) {
    EXPECT_EXIT(MetadataFailure(false, false, FaultMode::ReturnAppendError),
                ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, LockAwareReturnedMetadataFailureLatchesBeforeDiagnosticCopy) {
    EXPECT_EXIT(MetadataFailure(true, false, FaultMode::ReturnAppendError),
                ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, FallbackRemainsTerminalAfterRepeatedErrorsAndFailedObservers) {
    EXPECT_EXIT(TerminalDiagnosticFailure(true, false), ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, ExceptionFallbackSurvivesRepeatedErrorsAndFailedObservers) {
    EXPECT_EXIT(TerminalDiagnosticFailure(true, true), ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, DetailedFailureSurvivesRepeatedErrorsAndFailedObservers) {
    EXPECT_EXIT(TerminalDiagnosticFailure(false, false), ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, DetailedExceptionSurvivesRepeatedErrorsAndFailedObservers) {
    EXPECT_EXIT(TerminalDiagnosticFailure(false, true), ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, FallbackConstructionReportsAllocationFailureBeforeOwnerUse) {
    EXPECT_EXIT(FallbackConstructionFailure(), ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, StandaloneDurablePublicationRequiresNoFurtherAllocation) {
    EXPECT_EXIT(MetadataPublication(false), ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, LockAwareDurablePublicationRetainsReadPinsWithoutAllocation) {
    EXPECT_EXIT(MetadataPublication(true), ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, PreparedCompactPointerCopyFailureRemainsRetryable) {
    EXPECT_EXIT(PreparedPointerCopyFailure(), ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, PublicationRetainsLateRawPinsAndForegroundCounterAdvances) {
    EXPECT_EXIT(LatePinnedPublication(false), ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, RecoveredManifestPublishesAfterItsFinalCurrentNamespaceBarrier) {
    EXPECT_EXIT(LatePinnedPublication(true), ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, ExhaustedGroupLeaderWakesItsAdmittedFollower) {
    EXPECT_EXIT(GroupedFailure(), ::testing::ExitedWithCode(0), "");
}
TEST(WriteFailureDeathTest, ExhaustedReschedulingEpilogueSettlesTheAcceptedTask) {
    EXPECT_EXIT(ReschedulingFailure(), ::testing::ExitedWithCode(0), "");
}

}  // namespace
}  // namespace modern_leveldb
