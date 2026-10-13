# Learning Hard-Path Documentation Completion

## Scope

Complete the three maintainer-level learning gaps found after the current-tree
production review:

1. the `VersionSet` prepare, durable-write, publication, and retention state
   machine;
2. sharded-LRU reclamation when a user value destructor reenters the cache;
3. native file, read, mapping, lock, sync, and close lifecycles on POSIX and
   Windows.

This is a documentation-only change. It records current production behavior;
it does not change a public contract, a persistent format, or implementation
policy.

## Placement

| Gap | Maintained location | Reason |
|---|---|---|
| `VersionSet` transaction | `docs/learning/04-wal-and-recovery.md` | It is the in-memory half of the MANIFEST durability transaction already taught there. |
| Reentrant LRU reclamation | `docs/learning/05-tables-filters-and-caches.md` and Lab 6 | Membership, pins, eviction, and the relevant executable examples already live there. |
| Native resource lifecycles | New advanced lesson `docs/learning/12-native-filesystem-lifecycles.md` | The material crosses durability, ownership, copied reads, mappings, and locking; placing it in one existing lesson would overload that lesson or duplicate the model. |

Lesson 08 receives only the general lock rule that destruction invoking
external code must occur after unlocking. The learning README, concept index,
glossary, guided labs, and platform reference receive navigation or focused
cross-links rather than repeated implementations.

## Required content

### VersionSet state machine

The lesson must distinguish:

- validation and candidate construction from persistence;
- state captured before the database mutex is released from counters that may
  continue advancing while I/O runs;
- preallocation that makes post-durability publication non-allocating;
- first-MANIFEST creation and `CURRENT` installation from later MANIFEST
  appends;
- typed I/O failure, sticky edit rejection, and exceptions thrown before a
  typed uncertain-write result;
- publication of topology, compact pointers, and log counters only after
  durable metadata succeeds;
- `ReadPin`, shared Version owners, weak installed-Version tracking, and the
  live-file set used by cleanup.

The state diagram must show the database mutex released only around prepared
I/O and owned again on every return and exception path.

### Reentrant cache reclamation

The lesson must distinguish:

- indexed cache membership;
- intrusive references held by the cache and pins;
- a zero-reference entry retired under the shard mutex;
- value destruction and entry deallocation after the mutex is released.

It must explain that the typed value destructor is external code and may call
back into the cache. It must also preserve the deliberately surprising
replacement behavior: the old value's deleter can erase the new mapping before
`Insert` returns, while the returned pin still owns the replacement value.

### Native filesystem lifecycles

The advanced lesson must start from owners and invalidation events, not from a
list of system calls. It must cover:

- copied reader ownership and short-read contracts;
- Windows per-call `OVERLAPPED`/event/buffer ownership and terminal completion
  before return;
- POSIX and Windows mapped-view acquisition, fallback, rollback, and teardown;
- the different current unmap-failure policies;
- buffered writer sticky errors and close/error precedence;
- strict POSIX directory synchronization versus explicitly weak Windows
  namespace ordering;
- process-associated POSIX record locks and the process-local reservation
  table versus handle-associated Windows locks;
- immediate native-error capture before formatting or cleanup.

The platform reference remains the user-facing guarantee. The new lesson
explains why the implementation can satisfy that guarantee.

## Navigation and evidence

Each new section or lesson must:

- link to bounded current source entry points;
- name representative tests that exercise the difficult failure or lifetime
  edge;
- link to the current reference before ADR history;
- add self-check or lab questions that require reconstructing the invariant;
- avoid presenting historical ADR text as the maintained contract.

The concept index must give native lifecycle work its own route instead of
leaving maintainers to infer it from the generic durability row.

## Global review

After the focused additions, review every file under `docs/learning` plus
their directly linked architecture and platform-reference entry points for:

- prerequisite and first/implementation/deep-pass progression;
- consistent ownership, durability, error, pin, and publication terminology;
- duplication or disagreement between lessons;
- source/test links that still identify current responsibility boundaries;
- discoverability of advanced material without making it a first-pass
  prerequisite;
- stale claims contradicted by the current production tree.

Only directly related inconsistencies are in scope. Historical ADRs remain
history unless a current lesson incorrectly relies on a superseded mechanism.

## Validation

Run:

```console
python tools/check_documentation.py --root .
python -m unittest tests.tools.check_documentation_test
```

Then run the repository's complete focused checker selection if configured in
the existing build. Documentation validation must report no unclassified
documents, broken local links or anchors, malformed Markdown fences, stale
mutable evidence links, privacy findings, or line-ending violations.

## Acceptance criteria

- All three gaps have one authoritative learning location.
- The diagrams and explanations match current source and named tests.
- Existing first-pass lessons remain bounded and readable.
- The learning README and concept index make the additions discoverable.
- The global content review finds no remaining directly related
  contradiction or orphaned advanced topic.
- Documentation validation and checker tests pass.
