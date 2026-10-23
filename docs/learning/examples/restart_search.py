"""Entry-level teaching model, not an SST parser or database implementation."""

from collections.abc import Sequence
from dataclasses import dataclass


DEMO_KEYS = (
    b"ant", b"anteater", b"antelope", b"bat",
    b"batch", b"bath", b"cat", b"catalog",
    b"cater", b"dog", b"dove", b"eel",
)


@dataclass(frozen=True)
class EncodedBlock:
    entries: tuple[tuple[int, bytes], ...]
    restarts: tuple[int, ...]


@dataclass(frozen=True)
class SearchResult:
    index: int | None
    key: bytes | None
    restart_entry: int | None
    restart_comparisons: int
    decoded_entries: int


def encode_block(keys: Sequence[bytes], interval: int) -> EncodedBlock:
    """Construct the valid, bytewise-sorted immutable model used by the lesson."""
    if not isinstance(interval, int) or isinstance(interval, bool) or interval < 1:
        raise ValueError("restart interval must be a positive integer")
    keys = tuple(keys)
    if any(not isinstance(key, bytes) for key in keys):
        raise TypeError("keys must be bytes")
    if any(left >= right for left, right in zip(keys, keys[1:])):
        raise ValueError("keys must be strictly increasing")
    entries = []
    restarts = []
    previous = b""
    for index, key in enumerate(keys):
        shared = 0
        if index % interval == 0:
            restarts.append(index)
        else:
            while shared < min(len(previous), len(key)) and previous[shared] == key[shared]:
                shared += 1
        entries.append((shared, key[shared:]))
        previous = key
    return EncodedBlock(tuple(entries), tuple(restarts))


def fresh_lower_bound(block: EncodedBlock, target: bytes) -> SearchResult:
    """Requires an unchanged valid block from encode_block; starts unpositioned."""
    if not isinstance(target, bytes):
        raise TypeError("target must be bytes")
    if not block.entries:
        return SearchResult(None, None, None, 0, 0)
    left, right = 0, len(block.restarts) - 1
    comparisons = 0
    while left < right:
        middle = left + (right - left + 1) // 2
        restart_key = block.entries[block.restarts[middle]][1]
        comparisons += 1
        if restart_key < target:
            left = middle
        else:
            right = middle - 1
    restart_entry = block.restarts[left]
    previous = b""
    decoded = 0
    for index in range(restart_entry, len(block.entries)):
        shared, suffix = block.entries[index]
        key = previous[:shared] + suffix
        decoded += 1
        if key >= target:
            return SearchResult(index, key, restart_entry, comparisons, decoded)
        previous = key
    return SearchResult(None, None, restart_entry, comparisons, decoded)


if __name__ == "__main__":
    example = encode_block(DEMO_KEYS, 4)
    for query in (b"cat", b"cater", b"zoo"):
        result = fresh_lower_bound(example, query)
        print(f"{query!r}: key={result.key!r}, restart_entry={result.restart_entry}, "
              f"restart_comparisons={result.restart_comparisons}, "
              f"decoded={result.decoded_entries}")
