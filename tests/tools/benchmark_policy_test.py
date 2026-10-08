"""Native CMake/git reference identity and build-time policy refresh contracts."""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def git(root, *arguments):
    return subprocess.check_output(["git", "--no-pager", *arguments], cwd=root, text=True,
                                   stderr=subprocess.PIPE, timeout=15).strip()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--source", required=True)
    args = parser.parse_args()
    refresh = Path(args.source).resolve() / "cmake" / "RefreshBenchmarkPolicy.cmake"
    with tempfile.TemporaryDirectory(prefix="modern-policy-contract-") as temporary:
        root = Path(temporary)
        repository = root / "repository"
        repository.mkdir()
        git(repository, "init", "--quiet")
        git(repository, "config", "user.name", "Benchmark policy fixture")
        git(repository, "config", "user.email", "benchmark-policy@example.invalid")
        git(repository, "config", "commit.gpgsign", "false")
        (repository / ".gitignore").write_text("ignored/\n", encoding="utf-8")
        (repository / "source.cc").write_text("clean source\n", encoding="utf-8")
        git(repository, "add", "--all")
        git(repository, "commit", "--quiet", "-m", "fixture")
        revision = git(repository, "rev-parse", "HEAD")
        ignored = repository / "ignored"
        ignored.mkdir()
        base = root / "base.json"
        output = root / "policy.json"
        base.write_text(json.dumps({
            "reference_revision": "old", "reference_source": "old", "reference_dirty": "old"
        }), encoding="utf-8")

        def update(source):
            subprocess.run([
                args.cmake, f"-DREFERENCE_SOURCE={source}", "-DREFERENCE_OVERRIDE=ON",
                f"-DPOLICY_BASE={base}", f"-DPOLICY_OUTPUT={output}", "-P", str(refresh),
            ], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            return json.loads(output.read_text(encoding="utf-8"))

        clean = update(repository)
        assert clean["reference_revision"] == revision and clean["reference_dirty"] == "clean"
        external = update(ignored)
        assert external["reference_revision"] == "unknown"
        assert external["reference_source"] == "external-source-override"
        (repository / "source.cc").write_text("modified source\n", encoding="utf-8")
        dirty = update(repository)
        assert dirty["reference_revision"] == revision and dirty["reference_dirty"] == "modified"
        assert dirty["reference_source"] == "modified-git-source-override"
        (repository / "source.cc").write_text("clean source\n", encoding="utf-8")
        (repository / "untracked.txt").write_text("untracked\n", encoding="utf-8")
        assert update(repository)["reference_dirty"] == "modified"


if __name__ == "__main__":
    main()
