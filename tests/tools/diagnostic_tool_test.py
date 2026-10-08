"""Raw-byte native CLI evidence for closed-database diagnostic files."""

import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile


def run(arguments, **options):
    return subprocess.run(arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=30, **options)


def inventory(directory):
    return {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in directory.iterdir() if path.is_file()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True)
    parser.add_argument("--fixture", required=True)
    arguments = parser.parse_args()
    tool = str(Path(arguments.tool).resolve())
    fixture = str(Path(arguments.fixture).resolve())
    assert run([tool, "--help"]).returncode == 0
    usage = run([tool])
    assert usage.returncode == 2 and not usage.stdout
    assert usage.stderr == b"Usage: modern_leveldb_tool dump FILE...\n"
    with tempfile.TemporaryDirectory(prefix="modern-diagnostic-cli-") as root:
        directory = Path(root) / "\u6570\u636e-\U0001f4be space"
        created = run([fixture, str(directory)])
        assert created.returncode == 0, created.stderr
        before = inventory(directory)
        inputs = sorted(path for path in directory.iterdir()
                        if re.fullmatch(r"(MANIFEST-[0-9]+|[0-9]+\.(log|ldb))", path.name))
        assert {path.suffix for path in inputs} >= {".log", ".ldb"}
        assert any(path.name.startswith("MANIFEST-") for path in inputs)
        command = [tool, "dump", *map(str, inputs)]
        dumped = run(command)
        assert dumped.returncode == 0 and not dumped.stderr, dumped.stderr
        assert b"\r" not in dumped.stdout
        assert b"dump version=1 type=log " in dumped.stdout
        assert b"dump version=1 type=manifest " in dumped.stdout
        assert b"dump version=1 type=table " in dumped.stdout
        assert b"\\xe6\\x95\\xb0\\xe6\\x8d\\xae" in dumped.stdout
        assert b"user_key='key'" in dumped.stdout
        if os.name == "nt":
            extended = run([tool, "dump", *("\\\\?\\" + str(path.resolve()) for path in inputs)])
            assert extended.returncode == 0 and not extended.stderr, extended.stderr
            assert b"dump version=1 type=table " in extended.stdout
            assert b"user_key='key'" in extended.stdout
        destination = Path(root) / "captured.bin"
        with destination.open("wb") as output:
            redirected = subprocess.run(command, stdout=output, stderr=subprocess.PIPE, timeout=30)
        assert redirected.returncode == 0 and not redirected.stderr
        assert destination.read_bytes() == dumped.stdout
        mixed = run([tool, "dump", str(directory / "missing" / "000001.log"), str(inputs[-1])])
        assert mixed.returncode == 1 and b"not_found:" in mixed.stderr
        assert b"dump version=1 " in mixed.stdout
        rejected = run([tool, "dump", str(inputs[0]), "-invalid"])
        assert rejected.returncode == 2 and not rejected.stdout
        table = next(path for path in inputs if path.suffix == ".ldb")
        reader, writer = os.pipe()
        os.close(reader)
        try:
            process = subprocess.Popen([tool, "dump", str(table)], stdout=writer,
                                       stderr=subprocess.PIPE)
        finally:
            os.close(writer)
        try:
            _, errors = process.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate(timeout=5)
            raise
        assert process.returncode == 1 and errors
        assert inventory(directory) == before


if __name__ == "__main__":
    main()
