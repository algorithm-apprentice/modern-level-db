import contextlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import ci_changes


class WorkflowTriggersTest(unittest.TestCase):
    def test_runs_for_pull_requests_and_only_main_pushes(self):
        workflow = (
            Path(__file__).resolve().parents[2] / ".github" / "workflows" / "ci.yml"
        ).read_text(encoding="utf-8")
        triggers = workflow.partition("\non:\n")[2].partition("\npermissions:\n")[0]
        self.assertEqual(
            triggers,
            "  pull_request:\n  push:\n    branches:\n      - main\n",
        )


class DocumentationPathsTest(unittest.TestCase):
    def test_accepts_only_the_nonempty_documentation_allowlist(self):
        for paths in (
            ["README.md"], ["docs/documentation-manifest.json"],
            ["README.md", "docs/documentation-manifest.json", "docs/reference/README.md"],
            ["docs/code-style.md", "docs/adr/0048-documentation-only-ci.md"],
            ["docs/nested/space and\nnewline.md", "docs/unicode-\u03bb.md"],
        ):
            with self.subTest(paths=paths):
                self.assertTrue(ci_changes.documentation_only(paths))
        for path in (
            "src/table/block.cc", "tests/unit/table/block_test.cc", "tests/fixture.md",
            "CMakeLists.txt", ".github/workflows/ci.yml", "docs/tool.py", "README.MD",
            "docs/readme.MD", "docs/other.json", "docs/../src/example.md", "docs//example.md",
            "/docs/example.md", "./README.md", "docs.md",
        ):
            with self.subTest(path=path):
                self.assertFalse(ci_changes.documentation_only([path]))
                self.assertFalse(ci_changes.documentation_only(["README.md", path]))
        self.assertFalse(ci_changes.documentation_only([]))

    def test_considers_paths_beyond_remote_file_list_limits(self):
        paths = [f"docs/page-{index}.md" for index in range(3500)]
        self.assertTrue(ci_changes.documentation_only(paths))
        self.assertFalse(ci_changes.documentation_only(paths + ["src/changed.cc"]))

    def test_requires_complete_nul_delimited_paths(self):
        self.assertEqual(ci_changes.decode_paths(b""), [])
        self.assertEqual(ci_changes.decode_paths(b"README.md\0docs/with\nnewline.md\0"),
                         ["README.md", "docs/with\nnewline.md"])
        for malformed in (b"README.md", b"\0", b"README.md\0\0", b"docs/\xff.md\0"):
            with self.subTest(malformed=malformed), self.assertRaises(ValueError):
                ci_changes.decode_paths(malformed)


class GitChangeScopeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="modern-ci-diffs-")
        cls.repository = Path(cls.temporary.name)
        cls.git("init", "--quiet", "--initial-branch=main")
        cls.git("config", "user.name", "CI routing test")
        cls.git("config", "user.email", "ci-routing@example.invalid")
        cls.git("config", "commit.gpgsign", "false")
        cls.write("README.md", "initial readme")
        cls.write("docs/note.md", "initial note")
        cls.write("src/library.cc", "initial code")
        cls.base = cls.commit()
        cls.git("update-ref", "refs/remotes/origin/main", cls.base)
        cls.write("docs/note.md", "changed note")
        cls.docs_first = cls.commit()
        cls.write("README.md", "changed readme")
        cls.docs_second = cls.commit()
        cls.write("src/library.cc", "changed code")
        cls.code = cls.commit()
        cls.write("docs/note.md", "documentation after code")
        cls.code_then_docs = cls.commit()
        cls.git("mv", "src/library.cc", "docs/library.md")
        cls.code_to_docs = cls.commit()

        cls.git("switch", "--quiet", "-c", "base-advance", cls.base)
        cls.write("src/other.cc", "new base code")
        cls.advanced_base = cls.commit()
        cls.git("switch", "--quiet", "-c", "docs-renames", cls.docs_second)
        (cls.repository / "docs/nested").mkdir()
        cls.git("mv", "docs/note.md", "docs/nested/renamed.md")
        cls.docs_renamed = cls.commit()
        cls.git("rm", "--quiet", "README.md")
        cls.docs_deleted = cls.commit()
        cls.unusual_document = (
            "docs/space and unicode-\u03bb.md"
            if os.name == "nt" else
            "docs/space and\nnewline.md"
        )
        cls.write(cls.unusual_document, "unusual filename")
        cls.unusual_docs = cls.commit()
        (cls.repository / "tools").mkdir()
        cls.git("mv", "docs/nested/renamed.md", "tools/example.py")
        cls.docs_to_code = cls.commit()

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    @classmethod
    def git(cls, *arguments):
        return subprocess.check_output(
            ["git", "--no-pager", *arguments], cwd=cls.repository,
            stderr=subprocess.PIPE, text=True, timeout=10,
        ).strip()

    @classmethod
    def write(cls, path, contents):
        destination = cls.repository / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(contents)

    @classmethod
    def commit(cls):
        cls.git("add", "--all")
        cls.git("commit", "--quiet", "-m", "fixture")
        return cls.git("rev-parse", "HEAD")

    def push(self, before, after, ref="refs/heads/topic"):
        return {
            "before": before, "after": after, "ref": ref,
            "repository": {"default_branch": "main"},
        }

    def paths(self, event_name, event):
        return ci_changes.changed_paths(event_name, event, self.repository)

    def test_pr_uses_entire_branch_not_the_latest_documentation_commit(self):
        event = {"pull_request": {"base": {"sha": self.base}, "head": {"sha": self.code_then_docs}}}
        self.assertIn("src/library.cc", self.paths("pull_request", event))
        self.assertFalse(ci_changes.documentation_only(self.paths("pull_request", event)))
        incremental = self.paths("push", self.push(self.code, self.code_then_docs))
        self.assertEqual(incremental, ["docs/note.md"])
        self.assertTrue(ci_changes.documentation_only(incremental))

    def test_pr_does_not_treat_base_branch_advancement_as_pr_changes(self):
        event = {"pull_request": {
            "base": {"sha": self.advanced_base}, "head": {"sha": self.docs_second},
        }}
        self.assertEqual(set(self.paths("pull_request", event)), {"README.md", "docs/note.md"})

    def test_push_considers_all_commits_and_force_push_removals(self):
        self.assertEqual(set(self.paths("push", self.push(self.base, self.docs_second))),
                         {"README.md", "docs/note.md"})
        changed = self.paths("push", self.push(self.base, self.code_then_docs))
        self.assertIn("src/library.cc", changed)
        removed_code = self.paths("push", self.push(self.code_then_docs, self.docs_second))
        self.assertIn("src/library.cc", removed_code)
        self.assertFalse(ci_changes.documentation_only(removed_code))

    def test_new_branch_compares_against_fetched_default_branch(self):
        paths = self.paths("push", self.push("0" * 40, self.docs_second))
        self.assertEqual(set(paths), {"README.md", "docs/note.md"})
        paths = self.paths("push", self.push("0" * 40, self.code_then_docs))
        self.assertFalse(ci_changes.documentation_only(paths))

    def test_renames_include_both_paths_and_document_deletion_remains_documentation(self):
        for before, after, expected in (
            (self.code_then_docs, self.code_to_docs, {"src/library.cc", "docs/library.md"}),
            (self.docs_second, self.docs_renamed, {"docs/note.md", "docs/nested/renamed.md"}),
            (self.docs_renamed, self.docs_deleted, {"README.md"}),
            (self.unusual_docs, self.docs_to_code, {"docs/nested/renamed.md", "tools/example.py"}),
        ):
            with self.subTest(before=before, after=after):
                paths = self.paths("push", self.push(before, after))
                self.assertEqual(set(paths), expected)
                self.assertEqual(ci_changes.documentation_only(paths),
                                 expected.issubset({"README.md", "docs/note.md", "docs/nested/renamed.md"}))

    def test_unusual_git_filename_is_not_split_into_multiple_paths(self):
        paths = self.paths("push", self.push(self.docs_deleted, self.unusual_docs))
        self.assertEqual(paths, [self.unusual_document])

    def test_empty_diff_does_not_enable_fast_path(self):
        paths = self.paths("push", self.push(self.base, self.base))
        self.assertFalse(ci_changes.documentation_only(paths))

    def test_unknown_events_invalid_metadata_and_unavailable_revisions_are_explicit(self):
        for event_name, event in (
            ("workflow_dispatch", {}),
            ("push", self.push("0" * 40, self.base, "refs/heads/main")),
            ("push", self.push(self.base, self.docs_first, "refs/tags/v1")),
            ("push", self.push(self.base, "0" * 40)),
            ("push", self.push("--some-option", self.base)),
            ("pull_request", {"pull_request": {"base": {}, "head": {"sha": self.docs_first}}}),
        ):
            with self.subTest(event_name=event_name, event=event), self.assertRaises(
                (KeyError, ValueError)
            ):
                self.paths(event_name, event)
        with self.assertRaises(subprocess.CalledProcessError):
            self.paths("push", self.push("f" * 40, self.docs_first))

    def test_multiple_merge_bases_do_not_choose_one_arbitrarily(self):
        event = {"pull_request": {"base": {"sha": self.base}, "head": {"sha": self.docs_first}}}
        output = f"{self.base}\n{self.docs_first}\n".encode()
        with mock.patch.object(ci_changes, "git", return_value=output), self.assertRaises(ValueError):
            self.paths("pull_request", event)


class RoutingOutputTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="modern-ci-output-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.event = self.root / "event.json"
        self.event.write_text("{}")
        self.output = self.root / "output"
        self.environment = {
            "GITHUB_EVENT_NAME": "push", "GITHUB_EVENT_PATH": str(self.event),
            "GITHUB_OUTPUT": str(self.output),
        }
        self.messages = io.StringIO()

    def run_main(self):
        with mock.patch.dict(os.environ, self.environment), contextlib.redirect_stdout(self.messages), \
                contextlib.redirect_stderr(self.messages):
            return ci_changes.main()

    def test_emits_exact_decision_and_never_interpolates_filenames(self):
        with mock.patch.object(ci_changes, "changed_paths", return_value=["docs/\nignored.md"]):
            self.assertEqual(self.run_main(), 0)
        self.assertEqual(self.output.read_text(), "docs_only=true\n")
        self.assertNotIn("ignored.md", self.messages.getvalue())
        with mock.patch.object(ci_changes, "changed_paths", return_value=["src/library.cc"]):
            self.assertEqual(self.run_main(), 0)
        self.assertEqual(self.output.read_text(), "docs_only=true\ndocs_only=false\n")

    def test_comparison_failures_warn_and_select_full_ci(self):
        for failure in (KeyError("before"), ValueError("invalid metadata"),
                        subprocess.CalledProcessError(128, ["git", "diff"]),
                        subprocess.TimeoutExpired(["git", "diff"], 30), OSError("missing git")):
            with self.subTest(failure=failure):
                self.output.write_text("")
                with mock.patch.object(ci_changes, "changed_paths", side_effect=failure):
                    self.assertEqual(self.run_main(), 0)
                self.assertEqual(self.output.read_text(), "docs_only=false\n")
        self.assertIn("::warning::", self.messages.getvalue())

    def test_unreadable_or_malformed_event_warns_and_selects_full_ci(self):
        self.event.write_text("not json")
        self.assertEqual(self.run_main(), 0)
        self.assertEqual(self.output.read_text(), "docs_only=false\n")
        self.event.unlink()
        self.output.write_text("")
        self.assertEqual(self.run_main(), 0)
        self.assertEqual(self.output.read_text(), "docs_only=false\n")
        self.assertIn("::warning::", self.messages.getvalue())

    def test_output_failure_and_missing_workflow_environment_fail(self):
        self.environment["GITHUB_OUTPUT"] = str(self.root)
        with mock.patch.object(ci_changes, "changed_paths", return_value=["README.md"]):
            self.assertNotEqual(self.run_main(), 0)
        with mock.patch.dict(os.environ, {}, clear=True), contextlib.redirect_stderr(self.messages):
            self.assertNotEqual(ci_changes.main(), 0)


if __name__ == "__main__":
    unittest.main()
