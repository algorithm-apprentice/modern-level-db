# ADR-0074: Database Engine Coordination Structure

- Status: Accepted

## Context

`DatabaseEngine` is the coordination root for the database. It owns the mutex
that orders topology changes, the active WAL and memtables, the VersionSet,
snapshots, background maintenance, output protection, shutdown, and the sticky
failure that stops later writes.

The algorithms behind those responsibilities are already separated:

- recovery lives in `engine/recovery`;
- point lookup lives in `engine/lookup`;
- write grouping and batch insertion live in `engine/write_path`;
- memtable flushing lives in `engine/flush`;
- compaction picking and execution live in `engine/compaction_picker` and
  `engine/compaction`;
- seek charging lives in `engine/seek_statistics`.

At the time of this decision, the remaining engine implementation was
difficult to read as a bounded unit:

- `src/engine/database.h` has 251 lines;
- `src/engine/database.cc` has 712 lines;
- `DatabaseEngine` has approximately 33 direct data members;
- `tests/unit/engine/database_test.cc` has 1,999 lines.

The problem is not that one object coordinates the database. A storage engine
needs one owner for state transitions that cross WAL durability, memtable
visibility, VersionSet installation, background work, and file reclamation.
The problem is that the current implementation presents all of that state as
one flat member list and all coordination flows as one source file.

Several members also form stronger conceptual units than their declarations
show:

- the current WAL, its file number, the mutable memtable, and the published
  mutable-memory count describe one active write generation;
- the immutable memtable, retained read-pinned memtables, and the atomic
  immutable-presence mirror describe one flush generation;
- snapshot registration, duplicate registrations, and oldest-snapshot lookup
  form one registry;
- `background_error_` is not limited to background work because foreground
  commit and WAL-rotation failures also record it.

Reducing the raw member count is not itself the goal. Moving fields into empty
bags would only replace `member_` with `bag_.member_`. The design should expose
the state transitions and lifetime rules that a reader needs to understand.

## Concept and contract

```text
Concept:
    DatabaseEngine is the single coordination root for cross-component state.

Observed mismatch:
    Its algorithms are modular, but its coordination state and implementation
    flows are flattened into one class declaration and one source file.

Current authority:
    ADR-0037 defines DatabaseEngine behavior.
    ADR-0057 defines synchronous-read pin ownership.
    ADR-0004 defines ownership and runtime rules.

Invariant that must remain:
    Reorganization must not change public behavior, persistent formats,
    durability, visibility, failure stickiness, read ownership, synchronization,
    scheduling, or destruction order.
```

## Preserved invariants

Every implementation stage must preserve all of the following.

1. One database mutex protects topology, the write queue, snapshots, installed
   mutable and immutable state, pending outputs, seek statistics, scheduling,
   and the sticky error unless a field is explicitly documented as atomic.
2. Synchronous `Get` acquires and releases typed MemTable and Version read pins
   under that mutex, then performs table I/O without holding it.
3. Iterators and compactions retain shared ownership because they can outlive
   one engine call. Their lifetime model does not change.
4. Only one background task is scheduled at a time. Flush takes priority over
   compaction, and compaction can flush a newly pending immutable memtable
   between entries.
5. The first uncertain commit, WAL, MANIFEST, flush, compaction, or scheduling
   failure is sticky and stops later writes and maintenance.
6. A throwing old-WAL close leaves the installed WAL and memtable generation
   unchanged. A returned close error becomes sticky, but rotation still
   installs the already prepared replacement generation.
7. A complete write group becomes visible only after WAL work and every
   MemTable insertion complete, when the last sequence is published.
8. The mutable-memory diagnostic is published only at recovery, rotation, and
   successful complete-commit boundaries; it is not read concurrently from a
   mutating Arena.
9. An output file number remains protected from obsolete-file cleanup until
   its flush or compaction installation attempt is complete.
10. Obsolete-file cleanup retains every installed or pinned Version, every
    pending output, the required logs, and the current MANIFEST, and it remains
    disabled after uncertain metadata failure.
11. The background executor stops before objects used by its tasks are
    destroyed. Table and cache users are destroyed before their borrowed file
    system, cache, comparator, and directory storage. The directory lock is
    released after operational database state.
12. No production behavior depends on a new validation pass. Existing trusted
    internal boundaries remain trusted.

## Decision

### Keep DatabaseEngine as the coordination root

`DatabaseEngine` remains the only object that owns the database mutex and the
only layer that coordinates writes, reads, flush installation, compaction
installation, shutdown, and reclamation.

The restructuring will not create independently synchronized write,
background, or reclamation services. Such services would either duplicate
state or call back through the engine while sharing its mutex, making lock and
failure ownership harder to follow.

Algorithmic modules continue to accept explicit arguments and remain usable in
focused tests without constructing a complete database.

### Partition implementation by consumer flow

The class declaration remains in `engine/database.h`, but method definitions
are divided by the flow a contributor is following:

| File | Responsibility |
|---|---|
| `engine/database.cc` | option sanitization, owned-resource construction, `Open`, construction, destruction, and recovery installation |
| `engine/database_read.cc` | `ReadSources`, point reads, iterators, read sampling, snapshot entry points, and typed state inspection |
| `engine/database_write.cc` | write queue entry, room making, WAL/memtable rotation, group commit, and forced memtable flush entry |
| `engine/database_background.cc` | background waiting and scheduling, immutable flush installation, compaction, obsolete-file cleanup, shutdown checks, and sticky failure |

This split changes translation units, not runtime ownership. Private methods can
remain members when they coordinate several engine-owned states.

The private declaration in `database.h` follows the same order:

```text
lifecycle
    -> foreground writes
    -> foreground reads and snapshots
    -> background maintenance
    -> failure and reclamation
```

Tests are partitioned by the same concepts after production definitions move.
Shared test-only fixtures may live in a bounded
`tests/unit/engine/database_test_support.h`; the split must not create a general
test framework or duplicate setup across files.

### Name the database-wide sticky failure accurately

The following vocabulary changes without behavior change:

```text
background_error_       -> sticky_error_
RecordBackgroundError   -> RecordStickyError
BackgroundError         -> StickyError
```

The public diagnostic already calls this state `sticky_error`. The internal
name should match the invariant: foreground commit, WAL rotation, task
scheduling, flush, compaction, and MANIFEST installation can all make the
database uncertain.

`background_scheduled_` and `background_finished_` retain their names because
they describe actual background-task state.

### Extract SnapshotRegistry

Snapshot registration is an independent value-level policy that requires the
database mutex but no engine back-pointer. Add an internal
`SnapshotRegistry` with behavior equivalent to:

```cpp
class SnapshotRegistry final {
public:
    class Registration final {
    public:
        Registration(Registration&&) noexcept;
        [[nodiscard]] SequenceNumber sequence() const noexcept;
    };

    [[nodiscard]] Registration Register(SequenceNumber sequence);
    void Release(Registration& registration) noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::optional<SequenceNumber> oldest() const noexcept;
    [[nodiscard]] SequenceNumber OldestOr(SequenceNumber fallback) const noexcept;

private:
    struct Node {
        SequenceNumber sequence;
        Node* previous;
        std::unique_ptr<Node> next;
    };

    std::unique_ptr<Node> oldest_;
    Node* newest_;
};
```

The registry is not internally synchronized. DatabaseEngine calls it with the
database mutex held.

- each acquisition returns a move-only registration handle identifying one
  linked node, so duplicate sequences remain independent;
- nodes append in the nondecreasing order established by the VersionSet's last
  sequence;
- the forward `unique_ptr` chain owns every node; backward links and the newest
  pointer are non-owning;
- `Release` finds the owning forward link through the node's previous pointer,
  unlinks the exact node, and reconnects its successor in constant time;
- compaction asks for `OldestOr(last_sequence)`;
- state inspection asks for `size()` and `oldest()`.

The public RAII `SnapshotRegistration` stores this internal handle and exposes
its cached sequence to concurrent reads. This is an ownership-safe translation
of pinned LevelDB's linked `SnapshotList`: the registration still identifies
one exact node, while `unique_ptr` links replace owning raw pointers.

### Model the WAL and memtables as write-buffer state

Add an internal `WriteBufferState` that owns the state of the active write
generation and the one immutable generation:

```text
active generation
    current WAL owner
    current WAL number
    mutable MemTable owner
    published mutable-memory bytes

flush generation
    optional immutable MemTable owner
    owners retained only while synchronous reads hold typed pins
    atomic mirror indicating whether compaction should enter the mutex
```

The exact accessor names are implementation details, but the type owns these
operations and invariants:

- install the recovered WAL number, WAL owner, and new mutable MemTable as one
  active generation;
- expose the active WAL and MemTable to serialized commit;
- publish mutable-memory usage only after a complete successful commit;
- install a prepared replacement WAL and MemTable as one rotation, moving the
  previous mutable MemTable to immutable state;
- publish the immutable-presence atomic when rotation installs that state;
- retain a flushed immutable MemTable while synchronous read pins still use
  it, and release retained owners after pins drain;
- clear immutable state and its atomic mirror only after successful VersionSet
  installation;
- provide shared MemTable owners to long-lived iterators without changing
  their existing lifetime model.

`WriteBufferState` does not:

- allocate file numbers;
- open, sync, or close WAL files;
- decide whether a close error is sticky;
- schedule work;
- flush a MemTable to an SSTable;
- install a VersionEdit.

Those cross-component actions stay in DatabaseEngine. In particular,
`SwitchMemTable` still prepares replacements, syncs the new log name, calls
the old WAL's `Close`, applies the current exception/error policy, and only
then asks `WriteBufferState` to install the rotation. This keeps the
exception-safety boundary visible.

### Group immutable policy, keep resource lifetimes explicit

The four sanitized values retained for the database lifetime become one
private immutable policy value:

```cpp
struct EnginePolicy {
    std::size_t write_buffer_size;
    std::uint64_t max_file_size;
    bool sync_wal_creation;
    TableBuilderOptions table_options;
};
```

This is a semantic value: it is the engine's retained write and table policy,
not a generic bag.

The first implementation does not introduce a generic
`OwnedOrBorrowed<T>` abstraction for FileSystem, BlockCache, Clock, or
BackgroundExecutor. Their declaration order carries destruction semantics,
and hiding those relationships would be riskier than the duplicated
owner/pointer spelling.

After the behavioral state extractions, a separate review may propose a
resource holder only if it makes construction and destruction more explicit.
It is not required by this ADR.

### Preserve explicit atomic mirrors

`closing_` and the immutable-presence atomic remain explicit. They are cheap
signals observed while compaction is outside the database mutex:

- `closing_` stops per-entry compaction work and prevents installation after
  shutdown begins;
- immutable presence tells compaction whether it needs to enter the mutex and
  flush before processing another entry.

They are not treated as redundant copies of mutex-protected state. Their
comments must identify the authoritative protected state and the unlocked
consumer.

## Header shape after the change

The resulting declaration should read as a small number of responsibility
groups rather than as one undifferentiated list:

```text
owned long-lived resources
directory lock, declared early so it is released after operational state
borrowed stable services and table cache
immutable engine policy

database mutex and background notification
VersionSet
WriteBufferState and WriteQueue
SnapshotRegistry and read-sampling seed
pending outputs, seek statistics, sticky failure, scheduling, shutdown

owned executor and borrowed executor pointer, declared last so execution stops first
```

The exact top-level member count is not an acceptance criterion. A new type is
accepted only when it owns an invariant or state transition.

## Delivery

Implementation proceeds through sequential, exact-head pull requests:

1. **Partition implementation files.** Move method definitions and organize
   private declarations without renaming symbols or changing behavior.
2. **Partition database tests.** Move existing tests into lifecycle, read,
   write, and background files without changing assertions or fixtures beyond
   the minimum shared support.
3. **Adopt sticky-failure vocabulary.** Rename the field and helpers and
   reconcile comments and documentation.
4. **Extract SnapshotRegistry.** Add focused unit tests, then replace direct
   multiset access in DatabaseEngine.
5. **Extract WriteBufferState and EnginePolicy.** Preserve every recovery,
   rotation, read-pin, commit-publication, flush, and shutdown transition.
6. **Reconcile the final architecture.** Record the accepted member boundaries
   and implementation outcome in architecture and this ADR, then audit the
   complete concept index and learning source tours.

Each pull request is reviewed and merged before the next begins. Production
implementation does not start until this proposed ADR is approved.

Every pull request that moves a source or test entry point updates its direct
concept-index and source-tour links in that same revision. Step 6 is the final
cross-document audit, not permission to leave stale navigation between stages.

## Validation

### Design pull request

- documentation links and formatting;
- ADR index entry;
- review against ADR-0037, ADR-0057, and ADR-0004;
- no production or test-source changes.

### Implementation pull requests

At minimum, preserve or add focused evidence for:

- old-WAL close errors and exceptions during rotation;
- complete-group sequence publication;
- mutable-memory diagnostic publication during an unlocked commit;
- reads surviving mutable-to-immutable rotation and immutable installation;
- Version read pins surviving current-version replacement;
- duplicate snapshots and oldest-snapshot compaction visibility;
- sticky errors from foreground commit, scheduling, flush, compaction, and
  MANIFEST installation;
- pending-output retention and obsolete-file cleanup;
- compaction interruption by shutdown and by a pending immutable MemTable;
- executor shutdown and database destruction.

Every behavior-changing implementation revision runs:

- focused unit tests for the extracted type and affected engine flow;
- complete Debug and Release unit suites;
- compatibility, model, and crash suites;
- coverage, ASan, TSan, fuzz, benchmark, and retained performance CI;
- documentation checks;
- exact-head merge-tree verification.

The reorganization has no intended performance effect. Any measurable
regression in retained read, write, or compaction workloads is a defect, not
an accepted tradeoff.

## Alternatives rejected

### Keep the current single file and add section comments

Rejected. Comments help the member list but do not create bounded source and
test entry points. The current source and test files already exceed a useful
single-reading scope.

### Use PIMPL to shrink database.h

Rejected. `DatabaseEngine` is internal. PIMPL would hide the same state behind
one pointer, add indirection and allocation, and leave the ownership and lock
model unchanged.

### Create independently synchronized subsystem services

Rejected. Write rotation, flush installation, compaction, sticky failure,
pending outputs, and shutdown share one transaction boundary. Additional
mutexes would create ordering rules without creating independent ownership.

### Replace typed read pins with shared_ptr copies

Rejected. ADR-0057 deliberately removed per-Get shared ownership while keeping
long-lived iterator and compaction ownership unchanged. This restructuring is
not a performance-policy reversal.

### Move fields into passive state bags

Rejected. A passive aggregate reduces the visual member count but does not
centralize any invariant. SnapshotRegistry and WriteBufferState are accepted
because they own behavior and transitions.

### Introduce a generic owned-or-borrowed resource wrapper now

Rejected for the initial implementation. The current declaration order makes
shutdown dependencies visible. Resource wrapping may be reviewed separately
after the coordination state is simpler.

### Perform the reorganization in one pull request

Rejected. Mechanical file movement, terminology changes, and ownership-state
extraction have different review questions. Sequential changes preserve
blame, make failures attributable, and keep exact-head validation bounded.

## Design review

The design was checked against the current source, tests, and retained
decisions.

- ADR-0037 requires DatabaseEngine to own the state joining otherwise separate
  algorithms. The proposal retains that aggregate-root role.
- ADR-0057 requires typed synchronous read pins and shared ownership for
  longer-lived consumers. The proposal keeps both models.
- ADR-0004 requires shared ownership and performance abstractions to have
  explicit reasons. The proposal neither broadens shared ownership nor removes
  a measured hot-path mechanism by assumption.
- Pinned LevelDB's linked SnapshotList was reviewed directly. SnapshotRegistry
  preserves its registration identity and constant-time operations while
  replacing owning raw links with a forward `unique_ptr` chain.
- The current destructor and declaration order require the executor to stop
  first and resource owners to outlive their borrowers. The proposal leaves
  those lifetimes explicit.
- The current WAL-close exception repair requires replacement state not to be
  installed after a throw. WriteBufferState therefore owns only the final
  state installation, not the close operation or its policy.
- Background scheduling cannot become an independent object without sharing
  the engine mutex, VersionSet, and sticky failure. The proposal keeps it as
  engine coordination and limits the first step to source partitioning.
- A generic resource holder was removed from the initial scope because it
  reduced field count without yet improving a domain invariant.

No unresolved design question requires production experimentation. The next
gate is review and acceptance of this ADR.

## References

- [ADR-0004: Errors, Ownership, and Runtime Model](0004-errors-ownership-and-runtime.md)
- [ADR-0037: Database Engine](0037-database-engine.md)
- [ADR-0057: Pinned LevelDB Version and Output Parity](0057-leveldb-version-output-parity.md)
- [ADR-0061: Typed Database State Inspection](0061-typed-database-state-inspection.md)
- [Document-First Development](../development/document-first-development.md)
- [Pinned LevelDB SnapshotList](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/snapshot.h)
- [`src/engine/database.h`](../../src/engine/database.h)
- [`src/engine/database.cc`](../../src/engine/database.cc)
- [`tests/unit/engine/database_test_support.h`](../../tests/unit/engine/database_test_support.h)
- [`tests/unit/engine/database_lifecycle_test.cc`](../../tests/unit/engine/database_lifecycle_test.cc)
- [`tests/unit/engine/database_read_test.cc`](../../tests/unit/engine/database_read_test.cc)
- [`tests/unit/engine/database_write_test.cc`](../../tests/unit/engine/database_write_test.cc)
- [`tests/unit/engine/database_background_test.cc`](../../tests/unit/engine/database_background_test.cc)
