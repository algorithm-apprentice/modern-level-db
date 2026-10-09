#!/usr/bin/env python3
"""Select the documentation-only CI path without hiding code or missing checks."""

import json
import os
from pathlib import Path
import re
import subprocess
import sys


def documentation_only(paths):
    if not paths:
        return False
    for path in paths:
        if path in ("README.md", "docs/documentation-manifest.json"):
            continue
        if (not path.startswith("docs/") or not path.endswith(".md")
                or any(part in ("", ".", "..") for part in path.split("/"))):
            return False
    return True


def decode_paths(output):
    if not output:
        return []
    if not output.endswith(b"\0"):
        raise ValueError("Git returned an incomplete changed-path list")
    paths = output[:-1].decode("utf-8").split("\0")
    if any(not path for path in paths):
        raise ValueError("Git returned an empty changed path")
    return paths


def git(repository, *arguments):
    return subprocess.run(
        ["git", "--no-pager", *arguments], cwd=repository, check=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30,
    ).stdout


def commit_id(value):
    if (not isinstance(value, str) or re.fullmatch(r"[0-9a-fA-F]{40}", value) is None
            or value == "0" * 40):
        raise ValueError("event does not contain a valid nonzero commit identifier")
    return value


def merge_base(repository, base, head):
    bases = git(repository, "merge-base", "--all", base, head).decode("ascii").splitlines()
    if len(bases) != 1:
        raise ValueError("comparison does not have exactly one merge base")
    return commit_id(bases[0])


def changed_paths(event_name, event, repository=Path(".")):
    if event_name == "pull_request":
        request = event["pull_request"]
        head = commit_id(request["head"]["sha"])
        base = merge_base(repository, commit_id(request["base"]["sha"]), head)
    elif event_name == "push":
        ref = event["ref"]
        if not isinstance(ref, str) or not ref.startswith("refs/heads/"):
            raise ValueError("only branch pushes can use documentation-only routing")
        head = commit_id(event["after"])
        before = event["before"]
        if before == "0" * 40:
            default_branch = event["repository"]["default_branch"]
            if not isinstance(default_branch, str) or not default_branch:
                raise ValueError("new branch has no default-branch comparison")
            if ref == f"refs/heads/{default_branch}":
                raise ValueError("new default branch has no previous state")
            git(repository, "check-ref-format", f"refs/heads/{default_branch}")
            base = merge_base(repository, f"refs/remotes/origin/{default_branch}", head)
        else:
            base = commit_id(before)
    else:
        raise ValueError(f"unsupported CI event: {event_name}")
    return decode_paths(git(
        repository, "diff", "--no-ext-diff", "--no-textconv", "--no-renames",
        "--name-only", "-z", base, head, "--",
    ))


def main():
    try:
        event_name = os.environ["GITHUB_EVENT_NAME"]
        event_path = Path(os.environ["GITHUB_EVENT_PATH"])
        output_path = Path(os.environ["GITHUB_OUTPUT"])
    except KeyError as error:
        print(f"CI routing failed: missing workflow environment {error}", file=sys.stderr)
        return 1

    try:
        event = json.loads(event_path.read_text(encoding="utf-8"))
        paths = changed_paths(event_name, event)
        docs_only = documentation_only(paths)
        print(f"Changed paths: {len(paths)}; documentation-only: {str(docs_only).lower()}")
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print("::warning::Unable to establish a documentation-only change; running full CI.")
        print(json.dumps({"comparison_error": str(error)}))
        docs_only = False

    try:
        with output_path.open("a", encoding="utf-8") as output:
            output.write(f"docs_only={str(docs_only).lower()}\n")
    except OSError as error:
        print(f"CI routing failed to write its decision: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
