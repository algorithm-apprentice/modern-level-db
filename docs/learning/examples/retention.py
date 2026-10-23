"""One-key history model; not a compaction scheduler or persistent-file reader."""

from collections.abc import Sequence
from dataclasses import dataclass


@dataclass(frozen=True)
class Version:
    sequence: int
    value: bytes | None


def compact_key(records: Sequence[Version], oldest_snapshot: int,
                may_have_deeper: bool) -> tuple[Version, ...]:
    """Requires the caller's oldest-view and deeper-history assumptions to be true."""
    records = tuple(records)
    if (not isinstance(oldest_snapshot, int) or isinstance(oldest_snapshot, bool)
            or oldest_snapshot < 0):
        raise ValueError("oldest snapshot must be a nonnegative integer")
    if not isinstance(may_have_deeper, bool):
        raise TypeError("deeper-history possibility must be a boolean")
    if any(record.sequence < 0 for record in records):
        raise ValueError("sequences must be nonnegative")
    if any(left.sequence <= right.sequence for left, right in zip(records, records[1:])):
        raise ValueError("one-key records must have strictly decreasing sequences")
    kept = []
    previous_sequence = None
    for record in records:
        shadowed = previous_sequence is not None and previous_sequence <= oldest_snapshot
        obsolete_deletion = (record.value is None and record.sequence <= oldest_snapshot
                             and not may_have_deeper)
        if not shadowed and not obsolete_deletion:
            kept.append(record)
        previous_sequence = record.sequence
    return tuple(kept)


if __name__ == "__main__":
    history = tuple(Version(sequence, str(sequence).encode()) for sequence in (105, 101, 99, 70))
    print("oldest=100:", [v.sequence for v in compact_key(history, 100, False)])
    deletion = (Version(90, None), Version(70, b"old"))
    print("deeper possible:", compact_key(deletion, 100, True))
    print("no deeper history:", compact_key(deletion, 100, False))
