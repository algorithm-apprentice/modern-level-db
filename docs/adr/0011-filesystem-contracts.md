# ADR-0011: Filesystem Contracts and POSIX Backend

- Status: Accepted; amended on 2026-09-23 to remove `SequentialFile::Skip`
  after [ADR-0018](0018-wal-stream-io.md) rejected initial-offset WAL reads,
  its only planned caller
- Date: 2026-09-22

## Context

WAL, SSTable, MANIFEST, recovery, and file-lifecycle code require filesystem
operations with explicit error, ownership, concurrency, and durability
semantics. C++ `std::filesystem` provides paths and namespace operations, but
does not provide portable file-descriptor durability, positioned reads, or
database locking.

This ADR covers only the `implement-platform-fs` DAG node: internal file
interfaces and the first POSIX backend. Filename policy, WAL framing, atomic
metadata helpers, recovery, and Windows I/O remain separate nodes.

## Current Modern LevelDB callers

The interfaces are limited to known engine paths:

| Future caller | Required operations |
|---|---|
| WAL reader | Sequential read |
| WAL writer | Buffered append, flush, file sync, and close |
| SSTable reader | Concurrent positioned read and file size |
| SSTable writer | Buffered append, flush, file sync, and close |
| MANIFEST/CURRENT | Create/truncate or append, file sync, atomic rename, and directory sync |
| Recovery/cleanup | Existence, directory listing, size, rename, file removal, and empty-directory removal |
| DB open | Create database directory and acquire one non-blocking exclusive lock |
| Fault tests | Substitute or decorate filesystem and open-file interfaces |

No current caller requires asynchronous I/O, random writes, memory mapping,
direct I/O, scatter/gather I/O, remote filesystems, priorities, deadlines,
rate-limit hints, or a public VFS registry.

## Prior art and adopted decisions

### Google LevelDB `Env`

Adopt:

- Separate sequential, random-access, and writable file objects.
- Externally synchronized sequential/writable handles and concurrently safe
  random-access reads.
- 64 KiB buffered writable files.
- Retry of interrupted POSIX reads and writes.
- Non-blocking exclusive database locks with process-local duplicate-lock
  detection.
- File synchronization before metadata installation.

Change:

- Split filesystem concerns from clock, executor, and logging.
- Return RAII `std::unique_ptr` handles instead of owning raw pointers.
- Use caller-provided mutable byte views and explicit byte counts for reads.
- Use `std::filesystem::path`.
- Make directory synchronization explicit rather than detecting MANIFEST names
  inside a writable file.
- Make existence checks fallible so permission and I/O errors are not reported
  as “missing”.

### RocksDB `FileSystem`

Adopt the separation between the filesystem and open-file interfaces.
Deliberately reject its generalized `IOOptions`, priorities, temperatures,
property bags, async I/O, extensible registration, remote-storage hooks, and
debug contexts because current Modern LevelDB callers cannot use them.

### SQLite VFS

Adopt the idea that the storage engine accesses operating-system I/O only
through a narrow bottom-layer interface, enabling fault-injection shims.
Do not adopt runtime VFS registration or multiple lock strategies.

### POSIX

Adopt `open`, `read`, `pread`, `write`, `fdatasync`/`fsync`, `rename`,
directory `fsync`, `mkdir`, `unlink`, `rmdir`, `stat`, `opendir`, and
non-blocking `fcntl(F_SETLK)` locks.

POSIX `rename` atomically replaces an existing destination in the same
filesystem, but crash durability requires syncing the affected directory.
Likewise, `fsync` on a file does not make its directory entry durable.

## Decision

Add internal interfaces in the `platform` layer.

### Open-file interfaces

```cpp
class SequentialFile {
 public:
  SequentialFile() = default;
  SequentialFile(const SequentialFile&) = delete;
  SequentialFile& operator=(const SequentialFile&) = delete;
  SequentialFile(SequentialFile&&) = delete;
  SequentialFile& operator=(SequentialFile&&) = delete;
  virtual ~SequentialFile() = default;

  virtual Result<std::size_t> Read(MutableByteView output) = 0;
};

class RandomAccessFile {
 public:
  RandomAccessFile() = default;
  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;
  RandomAccessFile(RandomAccessFile&&) = delete;
  RandomAccessFile& operator=(RandomAccessFile&&) = delete;
  virtual ~RandomAccessFile() = default;

  virtual Result<std::size_t> Read(
      std::uint64_t offset, MutableByteView output) const = 0;
};

class WritableFile {
 public:
  WritableFile() = default;
  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;
  WritableFile(WritableFile&&) = delete;
  WritableFile& operator=(WritableFile&&) = delete;
  virtual ~WritableFile() = default;

  virtual Status Append(ByteView data) = 0;
  virtual Status Flush() = 0;
  virtual Status Sync() = 0;
  virtual Status Close() = 0;
};

class FileLock {
 public:
  FileLock() = default;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock(FileLock&&) = delete;
  FileLock& operator=(FileLock&&) = delete;
  virtual ~FileLock() = default;
};
```

All handle types are non-copyable and non-movable polymorphic interfaces.
Concrete handles own their operating-system resources.

### Filesystem interface

```cpp
class FileSystem {
 public:
  FileSystem() = default;
  FileSystem(const FileSystem&) = delete;
  FileSystem& operator=(const FileSystem&) = delete;
  FileSystem(FileSystem&&) = delete;
  FileSystem& operator=(FileSystem&&) = delete;
  virtual ~FileSystem() = default;

  virtual Result<std::unique_ptr<SequentialFile>> OpenSequential(
      const std::filesystem::path& path) = 0;
  virtual Result<std::unique_ptr<RandomAccessFile>> OpenRandomAccess(
      const std::filesystem::path& path) = 0;
  virtual Result<std::unique_ptr<WritableFile>> OpenWritable(
      const std::filesystem::path& path) = 0;
  virtual Result<std::unique_ptr<WritableFile>> OpenAppendable(
      const std::filesystem::path& path) = 0;

  virtual Result<bool> FileExists(const std::filesystem::path& path) const = 0;
  virtual Result<std::vector<std::filesystem::path>> ListDirectory(
      const std::filesystem::path& path) const = 0;
  virtual Result<std::uint64_t> FileSize(
      const std::filesystem::path& path) const = 0;

  virtual Status CreateDirectory(const std::filesystem::path& path) = 0;
  virtual Status RemoveFile(const std::filesystem::path& path) = 0;
  virtual Status RemoveDirectory(const std::filesystem::path& path) = 0;
  virtual Status RenameFile(
      const std::filesystem::path& source,
      const std::filesystem::path& destination) = 0;
  virtual Status SyncDirectory(const std::filesystem::path& path) = 0;
  virtual Result<std::unique_ptr<FileLock>> LockFile(
      const std::filesystem::path& path) = 0;
};
```

`FileSystem` methods are safe for concurrent calls. Returned sequential and
writable files require external synchronization. A random-access file supports
concurrent `Read` calls.

`FileSystem` and all open-handle interfaces have virtual destructors and are
non-copyable and non-movable. Implementations are owned through
`std::unique_ptr` or another explicit outer owner.

All operations reject an empty path or a native path containing an embedded
null byte with `InvalidArgument`. POSIX paths otherwise remain opaque native
byte sequences; this layer does not impose UTF-8 normalization.

### Read semantics

- `Read` writes at most `output.size()` bytes and returns the number written.
- An empty output view returns zero without issuing I/O.
- A short read, including zero at EOF, is successful.
- Each successful read operation performs one successful `read`/`pread`
  attempt after retrying `EINTR`; it does not loop to fill the caller's buffer.
- Sequential reads advance the file position by the returned byte count.
- Random reads do not mutate shared file position.
- On an error, the returned `Result` contains no byte count. Previously read
  file contents and the caller's buffer outside the bytes written by the
  failed system call are not given transactional guarantees.
- Offsets that cannot be represented by POSIX `off_t` return
  `InvalidArgument`.
- System-call byte counts are capped at `SSIZE_MAX` even if a larger
  `MutableByteView` is representable.

### Write and close semantics

- `Append` preserves byte order and either buffers or writes all input before
  returning success.
- The POSIX implementation uses the proven LevelDB 64 KiB buffering policy.
- Large remaining writes bypass the buffer after buffered bytes are flushed.
- Unbuffered writes retry `EINTR`, handle short writes, and treat a zero-byte
  write with data remaining as `Io` rather than looping forever.
- `Flush` moves user-space buffered bytes to the kernel. It is not durable.
- `Sync` flushes buffered bytes, then uses `fdatasync` where appropriate or
  `fsync`; macOS attempts `F_FULLFSYNC` before falling back.
- `Sync` synchronizes the file only. It never implicitly syncs a parent
  directory based on the filename.
- `Close` flushes, attempts `close` even after a flush failure, and returns the
  first observed error. It does not imply `Sync`.
- A writable file retains its first I/O error. Later `Append`, `Flush`, `Sync`,
  and the terminal `Close` call return that error; `Close` still attempts to
  release the descriptor.
- `close(EINTR)` is not retried because the descriptor's state is
  platform-dependent and retrying can close a reused descriptor.
- `Close` is a single terminal operation. Calling any file operation after
  `Close`, including `Close` again, is outside the interface contract.
- The destructor performs best-effort close because destructors cannot report
  I/O failures. It closes only when explicit `Close` was not attempted.
  Durable engine paths must call `Sync` and `Close` explicitly.
- A failed append or sync may leave partial file contents; higher-level WAL,
  table, and recovery protocols provide failure atomicity.

### Namespace and durability semantics

- `OpenWritable` creates or truncates a regular file.
- `OpenAppendable` creates a file or appends after its current contents.
- Opens use close-on-exec descriptors and mode `0644`, subject to the process
  umask. They do not create missing parent directories.
- `CreateDirectory` succeeds when the path already names a directory.
- `CreateDirectory` creates one directory with mode `0755`, subject to umask;
  it is not recursive and requires the parent to exist.
- `ListDirectory` returns child names relative to the requested directory and
  excludes `.` and `..`; ordering is unspecified.
- `RenameFile` uses atomic replacement where POSIX guarantees it. Modern
  LevelDB currently renames files within one database directory.
- `SyncDirectory` explicitly makes create, rename, and delete directory-entry
  changes durable where the filesystem supports directory `fsync`.
- Synchronization syscalls retry `EINTR`. On macOS, `F_FULLFSYNC` falls back to
  `fsync` only for unsupported-operation errors; media or I/O errors are
  returned rather than hidden by a weaker retry.
- Higher layers must use the order required by their protocol, typically:

```text
write file → Sync file → Close file → Rename file → SyncDirectory(parent)
```

Before a synced MANIFEST record first references a newly created WAL or
SSTable, the higher layer must complete this order:

```text
write new WAL/SST
→ Sync file
→ Close file
→ SyncDirectory(database)
→ append MANIFEST edit that references the file
→ Sync MANIFEST
```

This preserves the invariant that a durable MANIFEST never first references a
directory entry that may disappear after a crash. A new WAL that stays open
for appends is synced but not closed before the edit. Installing `CURRENT`
uses the separate temp-file/rename sequence and syncs the database directory
after the rename.

The filesystem layer exposes those primitives but does not invent
MANIFEST/CURRENT policy.

### Lock semantics

- `LockFile` creates the lock file if needed and attempts a non-blocking
  exclusive whole-file lock.
- Contention returns `Busy`.
- A process-global lock table rejects duplicate locks acquired through
  different `PosixFileSystem` instances when the same weakly canonical path
  is used, because traditional POSIX record locks are process-associated.
- Weak canonicalization resolves existing parent-directory symlinks even when
  the lock file has not been created, preventing real and symlinked DB paths
  from acquiring the same process-associated lock twice.
- `LockFile` normalizes and atomically reserves the process-local identity
  before opening the lock file. A rejected duplicate attempt must not open and
  close another descriptor for the same lock inode, because closing it could
  release the process's existing traditional `fcntl` lock.
- If open or kernel locking fails after reservation, the implementation closes
  its descriptor if needed and removes the reservation before returning.
- The returned RAII lock releases the kernel lock, closes its descriptor, and
  removes the process-local entry on destruction.
- The filesystem object and lock pathname remain valid independently because
  the lock owns its descriptor and path.

### Error model

- `ENOENT` and `ENOTDIR` map to `NotFound` when they mean the requested path
  cannot be resolved.
- Lock contention (`EACCES` or `EAGAIN`) maps to `Busy`.
- Empty/embedded-null paths, unrepresentable offsets, and existing
  non-directory targets passed to `CreateDirectory` map to `InvalidArgument`.
- Unsupported platform capabilities map to `NotSupported`.
- Other operating-system failures map to `Io` with operation, path, and
  system-error text.

Normal I/O errors return `Result`/`Status`. Allocation failures and C++ runtime
failures may throw according to ADR-0004.

## POSIX backend scope

The first implementation supports Linux and macOS. POSIX sources and tests are
compiled only on CMake `UNIX` platforms. Windows continues to compile the
portable interfaces but has no native backend until a future requirement and
ADR define one.

The initial backend does not add:

- `mmap`, direct I/O, read-ahead hints, descriptor limits, or file caches.
- Random writable files, truncation of open files, links, or recursive
  directory operations.
- Asynchronous operations, cancellation, deadlines, or priorities.
- A public default filesystem singleton or runtime VFS registry.
- Automatic directory syncing hidden inside writable-file methods.

## Fault-injection seam

All filesystem and open-file operations are virtual and return typed results.
This is the seam for later in-memory, tracing, and fault-injection
implementations. No forwarding-wrapper framework is added before a concrete
fault test needs it.

## Validation plan

Interface tests verify ownership traits and signatures on every platform.
POSIX tests use isolated temporary directories and cover:

- Empty, short, sequential, positioned, concurrent, and unaligned
  reads.
- Buffered, large, appendable, flushed, synced, closed, and reopened writes.
- Namespace operations, overwrite rename, directory listing, size, and
  explicit directory sync.
- Missing paths, invalid offsets, non-empty directory removal,
  embedded-null paths, and representative error mapping.
- Same-instance and cross-instance process-local lock contention and lock
  release through RAII.
- File-descriptor cleanup across repeated open/close cycles.

Durability ordering itself is validated later with fault injection and crash
tests when WAL and metadata protocols exist; this node verifies that the
required primitives are explicit and callable. Error paths that cannot be
induced portably, such as zero-progress regular-file writes or delayed
writeback failure, are exercised when the fault-injection filesystem is added;
the POSIX implementation still guards those paths explicitly.

## References

- [Google LevelDB `Env`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/include/leveldb/env.h)
- [Google LevelDB POSIX backend](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/util/env_posix.cc)
- [RocksDB `FileSystem`](https://github.com/facebook/rocksdb/blob/main/include/rocksdb/file_system.h)
- [SQLite VFS](https://www.sqlite.org/vfs.html)
- [POSIX `fsync`/`fdatasync`](https://man7.org/linux/man-pages/man2/fsync.2.html)
- [POSIX `rename`](https://man7.org/linux/man-pages/man2/rename.2.html)
- [POSIX record locking](https://man7.org/linux/man-pages/man2/fcntl_locking.2.html)
