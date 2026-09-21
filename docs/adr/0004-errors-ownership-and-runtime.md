# ADR-0004: Errors, Ownership, and Runtime Model

- Status: Accepted
- Date: 2026-09-21

## Context

Storage-engine failures are expected control-flow outcomes: missing keys,
truncated files, checksum failures, unavailable resources, and failed durable
synchronization. Ownership mistakes and ambiguous shutdown behavior can turn
these failures into corruption.

## Decision

- The minimum language version is C++23.
- Fallible value-returning operations use `std::expected<T, Error>`.
- Operations with no success value use `Status`, defined as
  `std::expected<void, Error>`.
- `ErrorCode` enumerators use PascalCase without a `k` prefix, for example
  `ErrorCode::NotFound`. Diagnostic code names such as `not_found` remain unchanged.
- Exceptions are not used for normal storage-engine control flow.
- Owning raw pointers are forbidden.
- Exclusive ownership uses values or `std::unique_ptr`.
- Shared ownership requires a documented lifetime reason and must not be the
  default.
- Borrowed byte input uses `std::span<const std::byte>`.
- Text input that is semantically text uses `std::string_view`.
- Paths use `std::filesystem::path`.
- Time uses `std::chrono` types.
- Background workers use `std::jthread` and `std::stop_token`.
- Coroutines are outside the initial implementation scope.

## Consequences

- Error propagation is explicit in function signatures.
- Lifetime and shutdown semantics can be tested without ad hoc callback
  ownership.
- Adapters may be needed for compression libraries and operating-system APIs.
- Performance-sensitive abstractions must be benchmarked rather than removed
  based on assumption.
