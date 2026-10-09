#!/usr/bin/env python3
"""Validate documentation structure and machine-referenced contracts."""

import argparse
from collections import Counter
from dataclasses import dataclass
import html
import json
from pathlib import Path, PurePosixPath
import re
import sys
from urllib.parse import unquote


ALLOWED_LIFECYCLES = {
    "Proposed",
    "Accepted",
    "Implemented",
    "Measurement-only",
    "Rejected",
    "Superseded",
}
MANIFEST_CLASSES = {
    "architecture",
    "compatibility-pointer",
    "decision-index",
    "decision-record",
    "delivery-dag",
    "development-reference",
    "entry-point",
    "historical-handoff",
    "learning",
    "user-reference",
}
MANIFEST_DISPOSITIONS = {
    "create",
    "move",
    "retain",
    "retain-with-current-reference",
    "rewrite",
}
MANIFEST_FIELDS = {
    "class",
    "lifecycle",
    "owner_node",
    "destination",
    "disposition",
    "compatibility",
}
EXPECTED_MANIFEST_RULES = {
    ("path", "README.md"): ("entry-point", "current"),
    ("path", "docs/architecture.md"): ("architecture", "current"),
    ("path", "docs/dependency-dag.md"): ("delivery-dag", "current"),
    ("path", "docs/code-style.md"): ("compatibility-pointer", "current"),
    ("glob", "docs/development/*.md"): ("development-reference", "current"),
    ("path", "docs/profiling-design.md"): ("historical-handoff", "historical"),
    ("path", "docs/write-profiling-design.md"): ("historical-handoff", "historical"),
    ("glob", "docs/reference/*.md"): ("user-reference", "current"),
    ("glob", "docs/learning/*.md"): ("learning", "current"),
    ("path", "docs/adr/README.md"): ("decision-index", "current"),
    ("glob", "docs/adr/*.md"): ("decision-record", "historical"),
}
MUTABLE_GITHUB_LINK = re.compile(
    r"https://github\.com/[^/\s)]+/[^/\s)]+/blob/(?:main|master)/[^\s)]+"
)
INLINE_LINK = re.compile(r"!?\[[^\]]*\]\(([^)\n]+)\)")
REFERENCE_LINK = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*(\S+)")
HEADING = re.compile(r"^(#{1,6})\s+(.+?)\s*#*$")
FENCE = re.compile(r"^\s{0,3}(`{3,}|~{3,})(.*)$")
ADR_FILE = re.compile(r"^(\d{4})-[a-z0-9][a-z0-9-]*\.md$")
ADR_TITLE = re.compile(r"^# ADR-(\d{4}): (.+)$")
ADR_INDEX_ROW = re.compile(
    r"^\| \[ADR-(\d{4}): ([^]]+)\]\((\d{4}-[^)]+\.md)\)"
    r" \| ([^|]+) \| ([^|]+) \|"
)
EMAIL = re.compile(r"\b[A-Za-z0-9._%+-]+@([A-Za-z0-9.-]+\.[A-Za-z]{2,})\b")
HOME_PATHS = (
    re.compile(r"(?i)\b[A-Z]:[\\/]+Users[\\/]+[^\s<>'\"]+"),
    re.compile(r"(?i)(?<![A-Za-z0-9_])/(?:Users|home)/[^\s<>'\"]+"),
)


@dataclass(frozen=True, order=True)
class Finding:
    path: str
    line: int
    message: str

    def render(self):
        return f"{self.path}:{self.line}: {self.message}"


@dataclass(frozen=True)
class MarkdownDocument:
    relative_path: str
    text: str
    anchors: frozenset


@dataclass(frozen=True)
class SchemaContract:
    label: str
    source: str
    pattern: str


@dataclass(frozen=True)
class ProcedureContract:
    name: str
    document: str
    document_markers: tuple
    workflow_job: str
    workflow_markers: tuple


SCHEMA_CONTRACTS = (
    SchemaContract(
        "Ordinary comparative samples",
        "benchmarks/db_bench.cc",
        r'\{\\"schema_version\\":(?P<version>\d+),\\"entries\\":',
    ),
    SchemaContract(
        "Read-family selected-workload completion",
        "benchmarks/profiling_bench.cc",
        r'\{\\"schema_version\\":(?P<version>\d+),\\"case\\":\\""'
        r' << selected_\.name\s+<< "\\",\\"preparations\\":1',
    ),
    SchemaContract(
        "Fixed-count read diagnostics",
        "benchmarks/profiling_bench.cc",
        r'output << "\{\\"schema_version\\":(?P<version>\d+),\\"case\\":";',
    ),
    SchemaContract(
        "Mutable selected-workload completion",
        "benchmarks/profiling_bench.cc",
        r'\{\\"schema_version\\":(?P<version>\d+),\\"case\\":\\""'
        r' << selected_\.name\s+<< "\\",\\"smoke\\":"',
    ),
    SchemaContract(
        "Native Windows stack report",
        "benchmarks/windows_cpu_profile.cc",
        r'\{\\"schema_version\\":(?P<version>\d+),\\"method\\":',
    ),
    SchemaContract(
        "Native Windows epoch ledger",
        "benchmarks/windows_profile_workload.h",
        r'\{\\"schema_version\\":(?P<version>\d+),\\"pid\\":',
    ),
)

PROCEDURE_CONTRACTS = (
    ProcedureContract(
        "POSIX debug build and test",
        "docs/reference/getting-started.md",
        (
            "cmake --preset dev-debug",
            "cmake --build --preset dev-debug",
            "ctest --preset dev-debug",
        ),
        "unit",
        (
            "ubuntu-latest",
            "macos-latest",
            "cmake --build build",
            "ctest --test-dir build",
        ),
    ),
    ProcedureContract(
        "Windows debug build and test",
        "docs/reference/getting-started.md",
        (
            "cmake --preset windows-debug",
            "cmake --build --preset windows-debug",
            "ctest --preset windows-debug",
        ),
        "unit",
        (
            "windows-latest",
            "cmake --build build",
            "ctest --test-dir build",
        ),
    ),
    ProcedureContract(
        "POSIX storage diagnostics",
        "docs/reference/storage-diagnostics.md",
        (
            "build/dev-debug/tools/modern_leveldb_tool",
            "modern_leveldb_tool dump",
        ),
        "unit",
        ("ubuntu-latest", "macos-latest", "ctest --test-dir build"),
    ),
    ProcedureContract(
        "Windows storage diagnostics",
        "docs/reference/storage-diagnostics.md",
        (
            r"build\windows-debug\tools\modern_leveldb_tool.exe",
            "modern_leveldb_tool dump",
        ),
        "unit",
        ("windows-latest", "ctest --test-dir build"),
    ),
    ProcedureContract(
        "POSIX compatibility",
        "docs/development/building-and-testing.md",
        (
            "cmake --preset compatibility",
            "cmake --build --preset compatibility --target modern_leveldb_extended_tests",
            "ctest --preset compatibility",
        ),
        "compatibility",
        (
            "cmake --preset compatibility",
            "cmake --build --preset compatibility --target modern_leveldb_extended_tests",
            "ctest --preset compatibility",
        ),
    ),
    ProcedureContract(
        "Windows compatibility",
        "docs/development/building-and-testing.md",
        (
            "cmake --preset windows-compatibility",
            "cmake --build --preset windows-compatibility --target modern_leveldb_extended_tests",
            "ctest --preset windows-compatibility",
        ),
        "windows-compatibility",
        (
            "cmake --preset windows-compatibility",
            "cmake --build --preset windows-compatibility --target "
            "modern_leveldb_extended_tests",
            "ctest --preset windows-compatibility",
        ),
    ),
    ProcedureContract(
        "POSIX ordinary benchmark",
        "docs/development/benchmarking-and-profiling.md",
        (
            "cmake --preset benchmarks",
            "cmake --build --preset benchmarks",
            "ctest --preset benchmarks",
        ),
        "benchmark",
        (
            "cmake --preset benchmarks",
            "cmake --build --preset benchmarks",
            "ctest --preset benchmarks",
        ),
    ),
    ProcedureContract(
        "Windows ordinary benchmark",
        "docs/development/benchmarking-and-profiling.md",
        (
            "cmake --preset windows-benchmarks",
            "cmake --build --preset windows-benchmarks",
            "ctest --preset windows-benchmarks",
        ),
        "windows-benchmark",
        (
            "cmake --preset windows-benchmarks",
            "cmake --build --preset windows-benchmarks",
            "ctest --preset windows-benchmarks",
        ),
    ),
    ProcedureContract(
        "POSIX selected-workload profiling",
        "docs/development/benchmarking-and-profiling.md",
        (
            "cmake --preset profiling",
            "cmake --build --preset profiling",
            "ctest --preset profiling",
        ),
        "performance",
        (
            "cmake --preset profiling",
            "cmake --build --preset profiling",
            "ctest --preset profiling",
        ),
    ),
    ProcedureContract(
        "Windows selected-workload profiling",
        "docs/development/benchmarking-and-profiling.md",
        (
            "cmake --preset windows-profiling",
            "cmake --build --preset windows-profiling",
            "ctest --preset windows-profiling",
        ),
        "windows-performance",
        (
            "cmake --preset windows-profiling",
            "cmake --build --preset windows-profiling",
            "ctest --preset windows-profiling",
        ),
    ),
)


def line_number(text, offset):
    return text.count("\n", 0, offset) + 1


def add_finding(findings, path, line, message):
    findings.append(Finding(path, line, message))


def read_utf8(root, relative_path, findings, document=False):
    path = root / relative_path
    try:
        data = path.read_bytes()
    except OSError as error:
        add_finding(findings, relative_path, 1, f"cannot read file: {error}")
        return None
    if data.startswith(b"\xef\xbb\xbf"):
        add_finding(findings, relative_path, 1, "UTF-8 BOM is not allowed")
    if document and b"\r" in data:
        add_finding(findings, relative_path, 1, "document must use LF line endings")
    if document and data and not data.endswith(b"\n"):
        add_finding(findings, relative_path, 1, "document must end with a newline")
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError as error:
        add_finding(findings, relative_path, error.start + 1, f"invalid UTF-8: {error}")
        return None


def markdown_files(root):
    files = []
    readme = root / "README.md"
    if readme.is_file():
        files.append(readme)
    docs = root / "docs"
    if docs.is_dir():
        files.extend(sorted(docs.rglob("*.md")))
    return files


def github_slug(value):
    value = html.unescape(value)
    value = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", value)
    value = re.sub(r"<[^>]+>", "", value)
    value = re.sub(r"[`*_~]", "", value).strip().casefold()
    value = "".join(character for character in value if character.isalnum()
                    or character in " -_")
    return re.sub(r"\s+", "-", value)


def strip_inline_code(line):
    return re.sub(r"(`+).*?\1", "", line)


def parse_link_target(raw_target):
    raw_target = raw_target.strip()
    if raw_target.startswith("<"):
        end = raw_target.find(">")
        return raw_target[1:end] if end >= 0 else raw_target
    return raw_target.split(maxsplit=1)[0]


def inspect_privacy(relative_path, text, findings):
    for pattern in HOME_PATHS:
        for match in pattern.finditer(text):
            add_finding(
                findings,
                relative_path,
                line_number(text, match.start()),
                "machine-specific home path is not allowed",
            )
    for match in EMAIL.finditer(text):
        domain = match.group(1).casefold()
        if domain in {"example.com", "example.invalid", "users.noreply.github.com"}:
            continue
        add_finding(
            findings,
            relative_path,
            line_number(text, match.start()),
            "personal email address is not allowed",
        )


def inspect_mutable_links(relative_path, text, findings):
    for match in MUTABLE_GITHUB_LINK.finditer(text):
        add_finding(
            findings,
            relative_path,
            line_number(text, match.start()),
            "mutable GitHub evidence link is not allowed",
        )


def parse_markdown(relative_path, text, findings):
    headings = []
    links = []
    fence_character = None
    fence_length = 0
    fence_line = 0
    previous_level = 0

    for number, line in enumerate(text.splitlines(), 1):
        fence = FENCE.match(line)
        if fence:
            marker = fence.group(1)
            remainder = fence.group(2).strip()
            if fence_character is None:
                fence_character = marker[0]
                fence_length = len(marker)
                fence_line = number
                if not remainder:
                    add_finding(findings, relative_path, number,
                                "opening code fence requires a language")
            elif marker[0] == fence_character and len(marker) >= fence_length and not remainder:
                fence_character = None
                fence_length = 0
                fence_line = 0
            continue
        if fence_character is not None:
            continue

        heading = HEADING.match(line)
        if heading:
            level = len(heading.group(1))
            title = heading.group(2).strip()
            anchor = github_slug(title)
            if previous_level and level > previous_level + 1:
                add_finding(
                    findings,
                    relative_path,
                    number,
                    f"heading level jumps from H{previous_level} to H{level}",
                )
            if not anchor:
                add_finding(findings, relative_path, number,
                            "heading does not produce a usable anchor")
            previous_level = level
            headings.append((anchor, number, title))

        content = strip_inline_code(line)
        for match in INLINE_LINK.finditer(content):
            links.append((number, parse_link_target(match.group(1))))
        reference = REFERENCE_LINK.match(content)
        if reference:
            links.append((number, parse_link_target(reference.group(1))))

    if fence_character is not None:
        add_finding(findings, relative_path, fence_line, "code fence is not closed")

    counts = Counter(anchor for anchor, _, _ in headings)
    for anchor, count in counts.items():
        if anchor and count > 1:
            lines = ", ".join(str(number) for value, number, _ in headings if value == anchor)
            add_finding(
                findings,
                relative_path,
                next(number for value, number, _ in headings if value == anchor),
                f"duplicate heading anchor '{anchor}' on lines {lines}",
            )
    return frozenset(anchor for anchor, _, _ in headings), links


def exact_case_path(root, candidate):
    try:
        relative = candidate.relative_to(root)
    except ValueError:
        return False
    current = root
    for part in relative.parts:
        try:
            names = {entry.name for entry in current.iterdir()}
        except OSError:
            return False
        if part not in names:
            return False
        current = current / part
    return current.exists()


def inspect_links(root, documents, links_by_document, findings):
    anchors = {
        (root / relative_path).resolve(): document.anchors
        for relative_path, document in documents.items()
    }
    for relative_path, links in links_by_document.items():
        source = root / relative_path
        for number, target in links:
            if (not target or target.startswith(("http://", "https://", "mailto:", "tel:"))
                    or target.startswith("//")):
                continue
            path_part, separator, fragment = target.partition("#")
            path_part = path_part.partition("?")[0]
            if "\\" in path_part:
                add_finding(findings, relative_path, number,
                            f"local Markdown link must use '/': {target}")
                continue
            destination = (
                (source.parent / unquote(path_part)).resolve()
                if path_part else source.resolve()
            )
            try:
                destination.relative_to(root)
            except ValueError:
                add_finding(findings, relative_path, number,
                            f"local link escapes the repository: {target}")
                continue
            if path_part and not exact_case_path(root, destination):
                add_finding(findings, relative_path, number,
                            f"local link target does not exist with exact case: {target}")
                continue
            if separator and fragment and destination.suffix.casefold() == ".md":
                expected = github_slug(unquote(fragment))
                if expected not in anchors.get(destination, frozenset()):
                    add_finding(findings, relative_path, number,
                                f"Markdown anchor does not exist: {target}")


def normalized_manifest_path(value, allow_glob=False):
    if not isinstance(value, str) or not value or "\\" in value or value.startswith("/"):
        return False
    parts = value.split("/")
    if any(part in {"", ".", ".."} for part in parts):
        return False
    if not allow_glob and any(character in value for character in "*?["):
        return False
    return True


def manifest_matches(relative_path, rule):
    if "path" in rule:
        return rule["path"] == relative_path
    pattern = rule.get("glob")
    return isinstance(pattern, str) and PurePosixPath(relative_path).match(pattern)


def inspect_manifest(root, document_paths, findings):
    relative_path = "docs/documentation-manifest.json"
    text = read_utf8(root, relative_path, findings, document=True)
    if text is None:
        return {}
    try:
        manifest = json.loads(text)
    except json.JSONDecodeError as error:
        add_finding(findings, relative_path, error.lineno, f"invalid JSON: {error.msg}")
        return {}
    if not isinstance(manifest, dict):
        add_finding(findings, relative_path, 1, "manifest root must be an object")
        return {}
    if manifest.get("schema_version") != 1:
        add_finding(findings, relative_path, 1, "manifest schema_version must be 1")
    if manifest.get("matching") != "first-match":
        add_finding(findings, relative_path, 1, "manifest matching must be 'first-match'")
    rules = manifest.get("rules")
    if not isinstance(rules, list) or not rules:
        add_finding(findings, relative_path, 1, "manifest rules must be a nonempty array")
        return {}
    future_roots = manifest.get("future_roots")
    if not isinstance(future_roots, list):
        add_finding(findings, relative_path, 1, "manifest future_roots must be an array")
    else:
        for index, value in enumerate(future_roots):
            if not normalized_manifest_path(value):
                add_finding(findings, relative_path, index + 1,
                            f"invalid manifest future root: {value!r}")

    seen_selectors = set()
    selector_rules = {}
    usable_rules = []
    for index, rule in enumerate(rules):
        line = index + 1
        if not isinstance(rule, dict):
            add_finding(findings, relative_path, line, "manifest rule must be an object")
            continue
        selectors = [name for name in ("path", "glob") if name in rule]
        if len(selectors) != 1:
            add_finding(findings, relative_path, line,
                        "manifest rule requires exactly one path or glob selector")
            continue
        selector = selectors[0]
        expected_fields = MANIFEST_FIELDS | {selector}
        if set(rule) != expected_fields:
            add_finding(findings, relative_path, line,
                        "manifest rule has missing or unknown fields")
        value = rule[selector]
        selector_valid = normalized_manifest_path(value, allow_glob=selector == "glob")
        if not selector_valid:
            add_finding(findings, relative_path, line,
                        f"invalid manifest {selector}: {value!r}")
        else:
            key = (selector, value)
            if key in seen_selectors:
                add_finding(findings, relative_path, line,
                            f"duplicate manifest selector: {value}")
            seen_selectors.add(key)
            selector_rules[key] = rule
        if rule.get("class") not in MANIFEST_CLASSES:
            add_finding(findings, relative_path, line,
                        f"unknown manifest class: {rule.get('class')!r}")
        if rule.get("lifecycle") not in {"current", "historical"}:
            add_finding(findings, relative_path, line,
                        f"invalid manifest lifecycle: {rule.get('lifecycle')!r}")
        if rule.get("disposition") not in MANIFEST_DISPOSITIONS:
            add_finding(findings, relative_path, line,
                        f"invalid manifest disposition: {rule.get('disposition')!r}")
        for field in ("owner_node", "compatibility"):
            if not isinstance(rule.get(field), str) or not rule[field]:
                add_finding(findings, relative_path, line,
                            f"manifest {field} must be a nonempty string")
        destination = rule.get("destination")
        if destination != "same-path" and not normalized_manifest_path(destination):
            add_finding(findings, relative_path, line,
                        f"invalid manifest destination: {destination!r}")
        if selector_valid:
            usable_rules.append((index, rule))

    for selector, expected in EXPECTED_MANIFEST_RULES.items():
        rule = selector_rules.get(selector)
        if rule is None:
            add_finding(findings, relative_path, 1,
                        f"required manifest selector is missing: {selector[1]}")
            continue
        actual = (rule.get("class"), rule.get("lifecycle"))
        if actual != expected:
            add_finding(
                findings,
                relative_path,
                1,
                f"manifest selector {selector[1]} must be {expected[0]}/{expected[1]}, "
                f"not {actual[0]}/{actual[1]}",
            )
    for selector in sorted(set(selector_rules) - set(EXPECTED_MANIFEST_RULES)):
        add_finding(findings, relative_path, 1,
                    f"manifest selector is not admitted by the quality contract: {selector[1]}")

    classifications = {}
    effective_counts = Counter()
    for path in document_paths:
        matches = [(index, rule) for index, rule in usable_rules if manifest_matches(path, rule)]
        if not matches:
            add_finding(findings, path, 1,
                        "Markdown file is not classified by the documentation manifest")
            continue
        index, rule = matches[0]
        classifications[path] = rule
        effective_counts[index] += 1
    for index, rule in usable_rules:
        if effective_counts[index] == 0:
            selector = rule.get("path", rule.get("glob"))
            add_finding(findings, relative_path, index + 1,
                        f"manifest rule has no effective Markdown match: {selector}")
    return classifications


def inspect_adrs(root, documents, classifications, findings):
    adr_directory = root / "docs" / "adr"
    if not adr_directory.is_dir():
        add_finding(findings, "docs/adr", 1, "ADR directory is missing")
        return
    records = []
    numbers = Counter()
    titles = Counter()
    for path in sorted(adr_directory.glob("*.md")):
        match = ADR_FILE.fullmatch(path.name)
        if not match:
            continue
        relative_path = path.relative_to(root).as_posix()
        document = documents.get(relative_path)
        if document is None:
            continue
        lines = document.text.splitlines()
        title = ADR_TITLE.fullmatch(lines[0]) if lines else None
        if title is None:
            add_finding(findings, relative_path, 1,
                        "ADR must begin with '# ADR-NNNN: Title'")
            continue
        number = title.group(1)
        if number != match.group(1):
            add_finding(findings, relative_path, 1,
                        "ADR title number does not match its filename")
        if not any(
            line.startswith("- Status:") or line in {"## Status", "## Status and scope"}
            for line in lines[1:12]
        ):
            add_finding(findings, relative_path, 2,
                        "ADR canonical status block is missing near the title")
        records.append((path.name, number, title.group(2), relative_path))
        numbers[number] += 1
        titles[title.group(2)] += 1
    for number, count in numbers.items():
        if count > 1:
            add_finding(findings, "docs/adr", 1, f"duplicate ADR number: {number}")
    for title, count in titles.items():
        if count > 1:
            add_finding(findings, "docs/adr", 1, f"duplicate ADR title: {title}")

    index_path = "docs/adr/README.md"
    index = documents.get(index_path)
    if index is None:
        add_finding(findings, index_path, 1, "ADR index is missing")
        return
    rows = []
    for number, line in enumerate(index.text.splitlines(), 1):
        match = ADR_INDEX_ROW.match(line)
        if match:
            rows.append((number, *match.groups()))
    targets = Counter(row[3] for row in rows)
    record_names = [record[0] for record in records]
    for name in record_names:
        if targets[name] != 1:
            add_finding(
                findings,
                index_path,
                1,
                f"ADR index requires exactly one primary row for {name}; found {targets[name]}",
            )
    extras = sorted(set(targets) - set(record_names))
    for name in extras:
        add_finding(findings, index_path, 1, f"ADR index targets unknown record: {name}")
    if [row[3] for row in rows] != record_names:
        add_finding(findings, index_path, 1,
                    "ADR index primary rows must follow ascending filename order")
    record_by_name = {record[0]: record for record in records}
    for line, number, title, target, _, lifecycle in rows:
        record = record_by_name.get(target)
        if record is None:
            continue
        if number != target[:4] or number != record[1] or title != record[2]:
            add_finding(findings, index_path, line,
                        "ADR index number/title does not match the target record")
        if lifecycle.strip() not in ALLOWED_LIFECYCLES:
            add_finding(findings, index_path, line,
                        f"ADR index has invalid lifecycle label: {lifecycle.strip()}")
    if classifications.get(index_path, {}).get("class") != "decision-index":
        add_finding(findings, index_path, 1,
                    "ADR index must be classified as decision-index")


def load_presets(root, findings):
    relative_path = "CMakePresets.json"
    text = read_utf8(root, relative_path, findings)
    if text is None:
        return {}, {}, {}
    try:
        presets = json.loads(text)
    except json.JSONDecodeError as error:
        add_finding(findings, relative_path, error.lineno, f"invalid JSON: {error.msg}")
        return {}, {}, {}
    if not isinstance(presets, dict):
        add_finding(findings, relative_path, 1, "CMake preset root must be an object")
        return {}, {}, {}
    result = []
    for key in ("configurePresets", "buildPresets", "testPresets"):
        entries = presets.get(key)
        names = {}
        if not isinstance(entries, list):
            add_finding(findings, relative_path, 1, f"{key} must be an array")
            result.append(names)
            continue
        for entry in entries:
            if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
                add_finding(findings, relative_path, 1, f"{key} entry requires a name")
                continue
            name = entry["name"]
            if name in names:
                add_finding(findings, relative_path, 1, f"duplicate preset name: {name}")
            names[name] = entry
        result.append(names)
    return tuple(result)


def cmake_targets(root):
    targets = set()
    pattern = re.compile(
        r"\b(?:add_executable|add_library|add_custom_target|modern_leveldb_add_library)"
        r'\s*\(\s*"?([A-Za-z0-9_.:+-]+)'
    )
    for path in root.rglob("CMakeLists.txt"):
        try:
            text = path.read_text(encoding="utf-8")
        except OSError:
            continue
        targets.update(pattern.findall(text))
    return targets


def inspect_current_commands(root, documents, classifications, findings):
    configure, build, test = load_presets(root, findings)
    targets = cmake_targets(root)
    binary_directories = {}
    for name, preset in configure.items():
        binary = preset.get("binaryDir")
        if isinstance(binary, str):
            match = re.fullmatch(r"\$\{sourceDir\}/build/([A-Za-z0-9_.-]+)", binary)
            if match:
                binary_directories[match.group(1)] = name

    command_patterns = (
        (re.compile(r"\bcmake\s+--preset\s+([A-Za-z0-9_.+-]+)"),
         configure, "configure preset"),
        (re.compile(r"\bcmake\s+--build\s+--preset\s+([A-Za-z0-9_.+-]+)"),
         build, "build preset"),
        (re.compile(r"\bctest\s+--preset\s+([A-Za-z0-9_.+-]+)"),
         test, "test preset"),
    )
    target_pattern = re.compile(r"--target\s+([A-Za-z0-9_.:+-]+)")
    python_pattern = re.compile(r"\bpython3?\s+([A-Za-z0-9_./-]+\.py)\b")
    generated_path = re.compile(
        r"\bbuild[\\/]([A-Za-z0-9_.-]+)[\\/](?:tools|benchmarks|fuzz)[\\/]"
    )
    for relative_path, document in documents.items():
        if classifications.get(relative_path, {}).get("lifecycle") != "current":
            continue
        for pattern, available, description in command_patterns:
            for match in pattern.finditer(document.text):
                name = match.group(1)
                if name not in available:
                    add_finding(
                        findings,
                        relative_path,
                        line_number(document.text, match.start()),
                        f"unknown current {description}: {name}",
                    )
        for match in target_pattern.finditer(document.text):
            name = match.group(1)
            if name not in targets:
                add_finding(
                    findings,
                    relative_path,
                    line_number(document.text, match.start()),
                    f"unknown documented CMake target: {name}",
                )
        for match in python_pattern.finditer(document.text):
            path = match.group(1)
            if not exact_case_path(root, (root / path).resolve()):
                add_finding(
                    findings,
                    relative_path,
                    line_number(document.text, match.start()),
                    f"documented Python script does not exist with exact case: {path}",
                )
        for match in generated_path.finditer(document.text):
            directory = match.group(1)
            if directory not in binary_directories:
                add_finding(
                    findings,
                    relative_path,
                    line_number(document.text, match.start()),
                    f"generated artifact path uses unknown preset build directory: {directory}",
                )
        if "modern_leveldb_tool" in document.text and "modern_leveldb_tool" not in targets:
            add_finding(findings, relative_path, 1,
                        "documented modern_leveldb_tool target is missing")


def inspect_schemas(root, documents, findings):
    relative_path = "docs/development/benchmarking-and-profiling.md"
    document = documents.get(relative_path)
    if document is None:
        add_finding(findings, relative_path, 1, "current schema reference is missing")
        return
    documented = {}
    schema_line = re.compile(r"^- ([^:]+): schema (\d+)\.$", re.MULTILINE)
    for match in schema_line.finditer(document.text):
        label = match.group(1)
        if label in documented:
            add_finding(findings, relative_path, line_number(document.text, match.start()),
                        f"duplicate documented schema label: {label}")
        documented[label] = (int(match.group(2)), line_number(document.text, match.start()))

    expected_labels = {contract.label for contract in SCHEMA_CONTRACTS}
    for label in sorted(set(documented) - expected_labels):
        add_finding(findings, relative_path, documented[label][1],
                    f"unknown documented current schema: {label}")
    for contract in SCHEMA_CONTRACTS:
        if contract.label not in documented:
            add_finding(findings, relative_path, 1,
                        f"missing documented current schema: {contract.label}")
            continue
        source = read_utf8(root, contract.source, findings)
        if source is None:
            continue
        matches = list(re.finditer(contract.pattern, source, re.DOTALL))
        if len(matches) != 1:
            add_finding(findings, contract.source, 1,
                        f"schema source pattern for {contract.label} matched {len(matches)} times")
            continue
        actual = int(matches[0].group("version"))
        stated, line = documented[contract.label]
        if stated != actual:
            add_finding(
                findings,
                relative_path,
                line,
                f"{contract.label} documents schema {stated}, but source emits {actual}",
            )


def workflow_jobs(text):
    matches = list(re.finditer(r"(?m)^  ([A-Za-z0-9_-]+):\s*$", text))
    jobs = {}
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        jobs[match.group(1)] = text[match.start():end]
    return jobs


def inspect_procedure_smoke_mapping(root, documents, findings):
    relative_path = ".github/workflows/ci.yml"
    workflow = read_utf8(root, relative_path, findings)
    if workflow is None:
        return
    jobs = workflow_jobs(workflow)
    guide_path = "docs/development/ci-and-quality-gates.md"
    guide = documents.get(guide_path)
    if guide is None:
        add_finding(findings, guide_path, 1, "CI and quality-gates guide is missing")
    for required in (
        "python3 tools/check_documentation.py",
        "python3 tests/tools/check_documentation_test.py",
    ):
        if required not in jobs.get("changes", ""):
            add_finding(findings, relative_path, 1,
                        f"changes job does not run required documentation gate: {required}")
    for contract in PROCEDURE_CONTRACTS:
        document = documents.get(contract.document)
        if document is None:
            add_finding(findings, contract.document, 1,
                        f"{contract.name} procedure document is missing")
            continue
        for marker in contract.document_markers:
            if marker not in document.text:
                add_finding(findings, contract.document, 1,
                            f"{contract.name} is missing documented marker: {marker}")
        job = jobs.get(contract.workflow_job)
        if job is None:
            add_finding(findings, relative_path, 1,
                        f"{contract.name} has no retained CI job: {contract.workflow_job}")
            continue
        for marker in contract.workflow_markers:
            if marker not in job:
                add_finding(
                    findings,
                    relative_path,
                    1,
                    f"{contract.name} CI job {contract.workflow_job} is missing marker: {marker}",
                )
        if guide is not None and f"`{contract.workflow_job}`" not in guide.text:
            add_finding(
                findings,
                guide_path,
                1,
                f"{contract.name} retained CI job is missing from the quality-gates guide: "
                f"{contract.workflow_job}",
            )


def check_repository(root):
    root = Path(root).resolve()
    findings = []
    files = markdown_files(root)
    if not files:
        return [Finding("README.md", 1, "no Markdown documentation was found")]

    documents = {}
    links_by_document = {}
    for path in files:
        relative_path = path.relative_to(root).as_posix()
        text = read_utf8(root, relative_path, findings, document=True)
        if text is None:
            continue
        inspect_privacy(relative_path, text, findings)
        inspect_mutable_links(relative_path, text, findings)
        anchors, links = parse_markdown(relative_path, text, findings)
        documents[relative_path] = MarkdownDocument(relative_path, text, anchors)
        links_by_document[relative_path] = links

    inspect_links(root, documents, links_by_document, findings)
    classifications = inspect_manifest(root, sorted(documents), findings)
    inspect_adrs(root, documents, classifications, findings)
    inspect_current_commands(root, documents, classifications, findings)
    inspect_schemas(root, documents, findings)
    inspect_procedure_smoke_mapping(root, documents, findings)
    return sorted(set(findings))


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="repository root (defaults to the parent of tools/)",
    )
    options = parser.parse_args(arguments)
    findings = check_repository(options.root)
    if findings:
        for finding in findings:
            print(finding.render(), file=sys.stderr)
        print(f"Documentation validation failed with {len(findings)} finding(s).",
              file=sys.stderr)
        return 1
    print("Documentation validation passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
