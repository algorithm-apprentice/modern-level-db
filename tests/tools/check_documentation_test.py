import contextlib
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import check_documentation


class DocumentationCheckTest(unittest.TestCase):
    def copy_repository(self):
        temporary = tempfile.TemporaryDirectory(prefix="modern-doc-check-")
        self.addCleanup(temporary.cleanup)
        destination = Path(temporary.name) / "repository"
        shutil.copytree(
            ROOT,
            destination,
            ignore=shutil.ignore_patterns(
                ".git", "build", "__pycache__", "*.pyc", ".venv", "out"
            ),
        )
        return destination

    @staticmethod
    def write(path, text):
        with path.open("w", encoding="utf-8", newline="\n") as output:
            output.write(text)

    @staticmethod
    def messages(repository):
        return [finding.message for finding in check_documentation.check_repository(repository)]

    def assert_has(self, messages, fragment):
        self.assertTrue(
            any(fragment in message for message in messages),
            f"missing finding containing {fragment!r}: {messages}",
        )

    def test_repository_passes(self):
        self.assertEqual(check_documentation.check_repository(ROOT), [])

    def test_reports_markdown_link_format_and_privacy_failures(self):
        repository = self.copy_repository()
        document = repository / "docs" / "reference" / "README.md"
        text = document.read_text(encoding="utf-8")
        text += (
            "\n## Repeated heading\n\n"
            "## Repeated heading\n\n"
            "[missing](missing.md#nowhere)\n\n"
            "[wrong case](readme.md)\n\n"
            "[missing anchor](README.md#not-a-heading)\n\n"
            "```\n"
            "unlabeled\n"
            "```\n\n"
            r"Private path: C:\Users\alice\workspace"
            "\n"
        )
        document.write_bytes(text.replace("\n", "\r\n").encode("utf-8"))
        messages = self.messages(repository)
        self.assert_has(messages, "document must use LF line endings")
        self.assert_has(messages, "duplicate heading anchor")
        self.assert_has(messages, "local link target does not exist")
        self.assert_has(messages, "local link target does not exist with exact case")
        self.assert_has(messages, "Markdown anchor does not exist")
        self.assert_has(messages, "opening code fence requires a language")
        self.assert_has(messages, "machine-specific home path is not allowed")

    def test_reports_manifest_and_adr_index_drift(self):
        repository = self.copy_repository()
        manifest_path = repository / "docs" / "documentation-manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["rules"] = [
            rule for rule in manifest["rules"]
            if rule.get("glob") != "docs/reference/*.md"
        ]
        self.write(manifest_path, json.dumps(manifest, indent=2) + "\n")

        index_path = repository / "docs" / "adr" / "README.md"
        lines = index_path.read_text(encoding="utf-8").splitlines()
        lines = [line for line in lines if not line.startswith("| [ADR-0001:")]
        self.write(index_path, "\n".join(lines) + "\n")

        messages = self.messages(repository)
        self.assert_has(messages, "not classified by the documentation manifest")
        self.assert_has(messages, "exactly one primary row for "
                        "0001-ground-up-cpp23-reimplementation.md")
        self.assert_has(messages, "primary rows must follow ascending filename order")

    def test_manifest_cannot_downgrade_current_documents(self):
        repository = self.copy_repository()
        manifest_path = repository / "docs" / "documentation-manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        reference = next(
            rule for rule in manifest["rules"]
            if rule.get("glob") == "docs/reference/*.md"
        )
        reference["class"] = "decision-record"
        reference["lifecycle"] = "historical"
        self.write(manifest_path, json.dumps(manifest, indent=2) + "\n")
        self.assert_has(
            self.messages(repository),
            "manifest selector docs/reference/*.md must be "
            "user-reference/current, not decision-record/historical",
        )

    def test_manifest_lifecycle_is_document_retention_not_decision_status(self):
        repository = self.copy_repository()
        manifest_path = repository / "docs" / "documentation-manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["lifecycle_dimension"]["name"] = "decision-status"
        self.write(manifest_path, json.dumps(manifest, indent=2) + "\n")
        self.assert_has(
            self.messages(repository),
            "manifest lifecycle_dimension must define document retention separately "
            "from ADR decision status",
        )

    def test_adr_status_is_exact_and_matches_the_index(self):
        repository = self.copy_repository()
        first = repository / "docs" / "adr" / "0001-ground-up-cpp23-reimplementation.md"
        self.write(
            first,
            first.read_text(encoding="utf-8").replace(
                "- Status: Accepted", "- Status: Accepted; implemented", 1
            ),
        )
        index = repository / "docs" / "adr" / "README.md"
        lines = index.read_text(encoding="utf-8").splitlines()
        lines = [
            line.replace("| Compatibility | Accepted |", "| Compatibility | Rejected |")
            if line.startswith("| [ADR-0002:") else line
            for line in lines
        ]
        self.write(index, "\n".join(lines) + "\n")
        messages = self.messages(repository)
        self.assert_has(messages, "ADR status must contain exactly one allowed lifecycle label")
        self.assert_has(
            messages,
            "ADR index lifecycle Rejected does not match "
            "0002-leveldb-format-compatibility.md status Accepted",
        )

    def test_reports_stale_current_commands_and_targets(self):
        repository = self.copy_repository()
        getting_started = repository / "docs" / "reference" / "getting-started.md"
        text = getting_started.read_text(encoding="utf-8")
        self.write(getting_started, text.replace(
            "cmake --preset dev-debug", "cmake --preset missing-debug", 1
        ))

        building = repository / "docs" / "development" / "building-and-testing.md"
        text = building.read_text(encoding="utf-8")
        self.write(building, text.replace(
            "modern_leveldb_extended_tests", "missing_extended_target", 1
        ))

        ci_guide = repository / "docs" / "development" / "ci-and-quality-gates.md"
        self.write(
            ci_guide,
            ci_guide.read_text(encoding="utf-8")
            + "\n```console\npython3 tools/missing_documentation_check.py\n```\n",
        )

        messages = self.messages(repository)
        self.assert_has(messages, "unknown current configure preset: missing-debug")
        self.assert_has(messages, "unknown documented CMake target: missing_extended_target")
        self.assert_has(messages, "documented Python script does not exist")

    def test_historical_documents_are_exempt_from_current_command_checks(self):
        repository = self.copy_repository()
        adr = repository / "docs" / "adr" / "0001-ground-up-cpp23-reimplementation.md"
        self.write(
            adr,
            adr.read_text(encoding="utf-8")
            + "\n```console\ncmake --preset historical-only\n"
            + "python3 tools/historical-only.py\n```\n",
        )
        self.assertEqual(self.messages(repository), [])

    def test_malformed_machine_metadata_reports_findings_without_crashing(self):
        repository = self.copy_repository()
        manifest_path = repository / "docs" / "documentation-manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["rules"][0]["path"] = 7
        self.write(manifest_path, json.dumps(manifest, indent=2) + "\n")
        self.write(repository / "CMakePresets.json", "[]\n")
        messages = self.messages(repository)
        self.assert_has(messages, "invalid manifest path: 7")
        self.assert_has(messages, "CMake preset root must be an object")

    def test_reports_documented_schema_drift(self):
        repository = self.copy_repository()
        guide = repository / "docs" / "development" / "benchmarking-and-profiling.md"
        text = guide.read_text(encoding="utf-8")
        self.write(guide, text.replace(
            "Fixed-count read diagnostics: schema 5.",
            "Fixed-count read diagnostics: schema 4.",
        ))
        self.assert_has(
            self.messages(repository),
            "Fixed-count read diagnostics documents schema 4, but source emits 5",
        )

    def test_reports_missing_platform_smoke_mapping(self):
        repository = self.copy_repository()
        workflow = repository / ".github" / "workflows" / "ci.yml"
        text = workflow.read_text(encoding="utf-8")
        self.write(workflow, text.replace(
            "  windows-performance:\n", "  windows-performance-disabled:\n", 1
        ))
        self.assert_has(
            self.messages(repository),
            "Windows selected-workload profiling has no retained CI job: "
            "windows-performance",
        )

    def test_rejects_new_mutable_upstream_links(self):
        repository = self.copy_repository()
        document = repository / "docs" / "reference" / "README.md"
        self.write(
            document,
            document.read_text(encoding="utf-8")
            + "\n[Mutable evidence](https://github.com/example/project/blob/main/source.cc)\n",
        )
        self.assert_has(
            self.messages(repository),
            "mutable GitHub evidence link is not allowed",
        )

    def test_cli_reports_success_and_failure(self):
        repository = self.copy_repository()
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            self.assertEqual(check_documentation.main(["--root", str(repository)]), 0)
        self.assertIn("Documentation validation passed.", output.getvalue())

        document = repository / "docs" / "reference" / "README.md"
        self.write(document, document.read_text(encoding="utf-8") + "\n[missing](absent.md)\n")
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            self.assertEqual(check_documentation.main(["--root", str(repository)]), 1)
        self.assertIn("Documentation validation failed", output.getvalue())


if __name__ == "__main__":
    unittest.main()
