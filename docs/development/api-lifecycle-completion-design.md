# Public API Object Lifecycle Completion

## Scope

Third ordered mainline task after native and engine completion. Source baseline:
`4cb61670bbbe0be8f7fe205802c1e449cedf5b23`.

The scope is the public Database, Iterator, Snapshot and WriteBatch wrappers,
their public headers, `database_state.h`, `options.h` and `api_internal.h`.
Eleven files contain 66 mechanical `function_definition` nodes and one lambda.

Tree-sitter misses four out-of-class defaulted move-assignment definitions:

- `Database::operator=(Database&&)`;
- `Iterator::operator=(Iterator&&)`;
- `Snapshot::operator=(Snapshot&&)`;
- `WriteBatch::operator=(WriteBatch&&)`.

The actual original inventory is therefore 70 C++ definitions and one lambda.
The seven recovery nodes in `db.h` are default-argument/type-identifier parser
gaps, not omitted definitions.

Complete scan evidence is retained in `api-src-index.json` and
`api-public-index.json`.

No public API, error text, move/copy semantics, snapshot registration, iterator
lifetime, batch bytes, engine options, comparator ownership, benchmark,
diagnostic or coverage policy changes.

## Ownership contracts

`Database` owns a shared `DatabaseState`; iterators and snapshot registrations
may retain it after the public Database handle is destroyed. `DatabaseState`
destroys the engine before releasing its retained comparator.

`Iterator::Impl` destroys its engine iterator before snapshot registration and
database state. `SnapshotRegistration` releases the engine registration before
dropping state. Snapshot validation remains shared by `Get` and `NewIterator`.

Moved-from Database, Iterator and WriteBatch operations keep their exact
InvalidArgument boundaries. `Write` copies a public batch;
`WriteExclusive` borrows its private encoding. Public getters continue to
return borrowed iterator views or owned vectors according to their existing
contracts.

Do not introduce a generic callback wrapper, base class or state-validation
framework around these small ownership boundaries.

## Remaining implementation frontier

`PublicState` currently combines fixed-level projection with scalar/optional
owner transfer. Extract file-local:

```text
DatabaseLevelState PublicLevelState(const DatabaseEngineLevelState&)
```

The helper maps one level's file count and byte total. `PublicState` retains:

1. the `DatabaseLevelCount == NumLevels` proof;
2. the fixed-level loop;
3. scalar and optional state copies;
4. final sticky `Error` owner transfer;
5. the owning `DatabaseState` return.

This is a representation leaf with no allocation, failure or policy. Do not
extract wrappers for individual scalar assignments or change `GetState`'s
one-expression transform lambda.

## Complete stopping boundaries

After this extraction:

- option compression validation/preparation remains a named composition;
- engine-option mapping is one public-to-engine representation operation;
- read-option preparation owns snapshot validation and sequence projection;
- Database methods are public moved-from/ownership boundary compositions;
- Iterator movement methods are direct moved-from checks plus one engine move;
- Snapshot special members are ownership leaves;
- WriteBatch copy-swap, move, mutation and size methods are bounded ownership or
  representation operations;
- internal state/registration/iterator owners are RAII leaves or direct
  accessors;
- public-header special members are compile-time ownership leaves.

No further helper improves responsibility clarity without hiding the public
ownership/error contract.

## Characterization and validation

Before production edits, strengthen
`PublicDatabaseTest.DatabaseStateOutlivesItsDatabase`:

1. use a small write buffer;
2. write enough data and trigger a memtable switch;
3. destroy the database after its background flush completes;
4. reopen it and capture public state;
5. prove at least one projected level has the exact nonzero file count/bytes;
6. destroy the reopened database and prove the copied public level state remains
   unchanged.

This directly characterizes the selected projection on unchanged production.
Retain all existing moved-from, foreign snapshot, iterator, batch, comparator,
native open/reopen and state ownership cases.

Fresh-build the public API test executable and freeze exact identities for
PublicOptionsTest, PublicWriteBatchTest, PublicDatabaseTest and
PublicWindowsDatabaseTest. Run the strengthened state case on unchanged
production before extraction.

After the production commit, require exact identity parity, Windows
Debug/Release, complete Windows Debug and Hosted POSIX/native parity. Authoritative
GCC 13 diff coverage remains unchanged.

## Final ledger requirement

The final scan must contain 71 definitions and one lambda: the 70 originals plus
`PublicLevelState`. Manually include the four defaulted move assignments and
resolve all seven recovery nodes. Every entry receives a reviewed composition
or responsibility-based leaf reason in session evidence.

API completion does not begin metadata/diagnostics work.

## Review focus

Review nonempty level projection, owner destruction order, moved-from
boundaries, snapshot/database identity checks, copy-versus-borrowed batch
submission, Error ownership and exact public test identity parity.
