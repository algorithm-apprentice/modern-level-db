# Storage Diagnostics

[User reference](README.md)

`modern_leveldb_tool dump` decodes canonical WAL, MANIFEST, and SSTable files
without opening a database through the public engine.

## Build the tool

Use the platform build in [Getting started](getting-started.md). Top-level
admitted Linux/macOS/Windows builds enable tools by default; set
`MODERN_LEVELDB_BUILD_TOOLS=OFF` to omit them.

| Platform | Typical executable |
|---|---|
| Linux/macOS Debug preset | `build/dev-debug/tools/modern_leveldb_tool` |
| Windows Debug preset | `build\windows-debug\tools\modern_leveldb_tool.exe` |

## Commands

```text
modern_leveldb_tool --help
modern_leveldb_tool dump FILE...
```

Unknown commands/options or missing dump files print usage to stderr and exit
with status 2. `--help` prints usage to stdout and exits with status 0. Dump
returns status 1 after an input or output failure. Input-file errors are
reported while later files continue; an output failure stops the command.

## Safety boundary

Use the tool only with:

- files from a closed database;
- stable test fixtures;
- a consistent offline copy.

Do not redirect stdout onto an input file or any database file. The tool is
read-only by design, but shell redirection can still overwrite data before the
tool reads it.

Diagnostic output may contain sensitive application keys and values. Treat it
as application data, not harmless logs.

## Output contract

The version-1 escaped text schema:

- identifies the input file;
- preserves binary keys/values through escaping;
- reports corruption and trailing/truncated input explicitly;
- is intended for inspection and test evidence.

It is not a backup, restore, repair, migration, or stable machine API. There
is no JSON mode, recursion, key filter, output-file option, comparator plugin,
or repair behavior.

Windows uses a wide command entry point and exact ASCII/LF output bytes through
checked native handles. It does not route database paths through the ANSI code
page or text-mode CRLF translation. Windows SSTable decoding can use a
read-only file mapping and can therefore terminate on an in-page storage
fault. The POSIX diagnostic deliberately uses copied reads and reports typed
read errors.
