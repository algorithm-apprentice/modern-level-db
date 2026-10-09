# ADR-0070: Documentation Information Architecture

## Status and scope

Proposed after the complete 88-file professional documentation audit on
2026-10-09. This design is the first node in the documentation-remediation
DAG. Broad rewrites do not begin until this ADR is reviewed and merged.

The audit found no P1 blocker and strong baseline formatting, safety language,
learning progression, and later-ADR outcomes. It also found current guidance
mixed with historical implementation plans, a monolithic README, missing ADR
navigation, stale cross-platform examples, and authoritative architecture
documents that no longer match the module graph.

This ADR defines audiences, document classes, sources of truth, navigation,
status/supersession rules, migration order, and automated quality boundaries.
It does not rewrite the 24,000-line corpus, change product behavior, or
introduce a documentation-site generator.

## Audiences and entry paths

Documentation must identify the reader and the decision they are trying to
make. The same fact should not be normatively maintained in several places.

| Audience | Primary question | Entry point |
|---|---|---|
| Evaluator | What is this project, what works, and what are the risks? | Root `README.md` |
| Library user | How do I consume, configure, and operate the library safely? | `docs/reference/README.md` |
| Contributor | How do I build, test, measure, review, and deliver changes? | `docs/development/README.md` |
| Architecture reader | How is the current implementation partitioned? | `docs/architecture.md` and `docs/dependency-dag.md` |
| Learner | How does an LSM engine work in this codebase? | `docs/learning/README.md` |
| Decision reviewer | Why was a mechanism chosen, changed, or rejected? | `docs/adr/README.md` |

The README routes readers to those paths. It is not the complete user manual,
developer handbook, benchmark specification, release history, or ADR index.

## Document classes and authority

### Root README

The README is a concise, current landing page:

- project purpose and pre-alpha warning;
- capability/platform support matrix;
- five-minute build and consumption example;
- the most important durability and mapped-read safety boundaries;
- links to current reference, development, architecture, learning, and ADR
  documentation.

Detailed CI matrices, profiling schemas, historical delivery narratives, and
long platform procedures belong in their authoritative guides.

### Current user reference

`docs/reference/` owns current externally visible behavior:

- supported platforms/toolchains/storage and explicit non-goals;
- prerequisites, dependency downloads, consumption, and quick start;
- public API, options, ownership and lifetime behavior;
- durability, crash, mapped-page, and external-mutation boundaries;
- diagnostics and safe offline operation.

Public headers remain the compile-time contract. Reference prose explains how
to use that contract and links to the exact headers. An ADR may explain why an
API exists but is not the current how-to guide.

### Current development reference

`docs/development/` owns current contributor procedures:

- supported configure/build/test presets;
- CI routing and quality gates;
- benchmark, diagnostics, profiling, artifact, and provenance procedures;
- code/documentation style and sequential PR delivery.

Research handoffs and measurement histories remain available but link to the
current procedure and identify themselves as historical evidence.

### Current architecture

`docs/architecture.md` is the normative description of the implemented
module/layer graph, source layout, and dependency direction. It is checked
against CMake/source dependencies. `docs/dependency-dag.md` owns delivery-node
prerequisites, canonical execution order, and completion state; it links to
the architecture instead of maintaining a second conflicting module graph.
Historical target layouts remain in ADR-0003 or other dated decisions.

### Learning path

`docs/learning/` remains a progressive curriculum. Lessons teach concepts and
use current cross-platform behavior, runnable paths, and stable source tours.
They link to current reference for operational details and to indexed ADRs for
decision history. A lesson does not become a second normative API or build
guide.

### Architecture decision records

`docs/adr/` is immutable decision history with maintained lifecycle metadata.
The canonical metadata is the status block at the top of each ADR. The index
is derived from those blocks and is not a second authority.

Allowed lifecycle labels are:

- `Proposed`
- `Accepted`
- `Implemented`
- `Measurement-only`
- `Rejected`
- `Superseded`

The metadata may also record `implemented_by`, `supersedes`,
`superseded_by`, and `amended_by`. Accepted ADR bodies are historical records:
later maintenance changes metadata, corrects broken references, or appends a
clearly dated outcome; it does not rewrite the original decision as if later
knowledge existed.

The maintained metadata includes:

- title and number;
- status;
- accepted/implemented PR where applicable;
- amendments;
- superseding and superseded decisions;
- current-reference link where the operational contract moved.

Historical bodies are not rewritten to pretend they were designed with later
knowledge. Status/outcome notes prevent old mechanisms from appearing current.
`docs/adr/README.md` provides a searchable index by topic and lifecycle.

## Source-of-truth matrix

| Information | Normative source | Other documents may |
|---|---|---|
| Public signatures/defaults | Public headers | Explain and link |
| Supported platforms and safety boundaries | `docs/reference/` | Summarize with a link |
| Preset/target/schema facts | Presets, CMake targets, and executable validators | Procedures explain and link |
| Build/test procedures | `docs/development/` | Show a minimal subset |
| Current module graph | `docs/architecture.md` | Explain one flow |
| Delivery prerequisites/order | `docs/dependency-dag.md` | Summarize completion |
| Benchmark/report schemas | Executable validators | Development guide documents current use; ADRs retain history |
| Decision rationale/history | ADRs | Summarize outcome |
| Teaching sequence | Learning path | Link to lessons |
| Project overview | README | Route to authoritative detail |

When code and prose disagree, the mismatch is a documentation defect; it is
not resolved by declaring every implementation detail the permanent contract.

## Current versus historical language

Current reference uses present tense and exact supported behavior. Historical
documents use dated framing:

- "At this design point ..."
- "This initial slice ..."
- "Superseded by ADR-..."
- "The current procedure is documented in ..."

Words such as "planned," "proposed," "future," and "not implemented" require a
dated status or current verification. Versioned schema documentation presents
the current schema first; legacy schemas are clearly marked historical.

## Command and example contract

Every primary command sequence states or inherits an explicit:

- operating system/platform;
- shell syntax;
- working directory;
- prerequisite toolchain;
- expected created files/directories;
- cleanup/safety boundary.

Examples must run as written on their declared platform. Cross-platform
examples either use portable behavior or present separate commands. Windows
database examples include weak-namespace consent next to `Database::Open`.
Durability text distinguishes file-content sync from namespace durability.

## Migration manifest

`docs/documentation-manifest.json` classifies every audited Markdown file
through ordered first-match rules. Specific paths precede broader globs; the
quality gate reports unmatched files and unintended shadowing. Each rule records:

- path or glob;
- document class;
- lifecycle (`current` or `historical`);
- remediation owner node;
- destination;
- disposition (`create`, `retain`, `rewrite`, `move`, or
  `retain-with-current-reference`);
- link/anchor compatibility policy.

Quality automation must prove that every Markdown file matches exactly one
rule. A move requires an intentional compatibility decision: preserve the old
path with a short pointer when external/deep links are plausible, or update
all repository links in the same PR. The manifest evolves with the migration
and prevents files from silently falling between task boundaries.

## Navigation and discoverability

- The README links to one documentation hub for each audience.
- `docs/reference/README.md` and `docs/development/README.md` index current
  operational documents.
- `docs/learning/README.md` retains the curriculum and links ADRs through the
  ADR index.
- `docs/adr/README.md` lists every ADR and its lifecycle.
- Every non-ADR document is reachable from a primary index.

Directories alone are not indexes for a corpus of dozens of documents.

## Style and formatting

Retain the existing strengths:

- English prose and identifiers;
- UTF-8/LF files with a final newline;
- sentence-case descriptive headings;
- language-tagged code fences;
- concise paragraphs, scannable lists, and meaningful table headers;
- explicit warnings and nonclaims near the action that needs them;
- no personal paths, credentials, or private identity.

Avoid:

- chronological component inventories in entry-point prose;
- repeating the same normative command in several documents;
- unexplained acronyms before their first definition;
- duplicate heading anchors;
- mutable upstream evidence links when an immutable revision is known.

## Automated quality gates

Common checks apply to every document class:

- relative link targets and Markdown anchors;
- heading hierarchy and duplicate anchors;
- balanced, language-tagged fences;
- UTF-8/LF/final newline and privacy patterns;
- manifest classification;
- ADR index coverage and unique ADR numbers.

Current reference, development, architecture, learning, and README documents
also receive current-preset/target/schema checks. Historical ADR and handoff
bodies are exempt from present-tense/current-command rules but not from links,
anchors, metadata, encoding, privacy, or fence checks.

Documentation-only CI runs dependency-free structural and static command
validation. Commands requiring platform toolchains, dependency downloads, or
product execution run in their existing Linux/macOS/Windows jobs; the
documentation gate verifies that those retained smoke cases cover every
primary platform procedure.

Automation enforces objective structure. Professional review remains
responsible for audience fit, accuracy, hierarchy, duplication, safety, and
maintainability.

## Migration DAG and canonical order

The remediation uses sequential PRs:

1. `docs-information-architecture`
2. `docs-user-reference`
3. `docs-development-reference`
4. `docs-architecture-refresh`
5. `docs-adr-governance`
6. `docs-quality-automation`
7. `docs-learning-cross-platform`
8. `docs-readme-rewrite`
9. `docs-final-editorial-review`

The dependency edges are:

| Node | Direct prerequisites | Objective exit evidence |
|---|---|---|
| `docs-information-architecture` | None | Reviewed ADR, manifest coverage, finding map |
| `docs-user-reference` | Information architecture | Runnable POSIX/Windows quick starts; API/platform/safety reference |
| `docs-development-reference` | Information architecture | Current build/test/CI/measurement procedures; legacy handoffs marked |
| `docs-architecture-refresh` | Information architecture | Module graph and source layout checked against CMake/source |
| `docs-adr-governance` | Information architecture | Complete derived index; lifecycle/supersession/anchor/link corrections |
| `docs-quality-automation` | User reference, development reference, architecture refresh, ADR governance | Dependency-free static gate plus retained platform smoke mapping |
| `docs-learning-cross-platform` | User reference, development reference, architecture refresh, ADR governance, quality automation | Labs run from declared directories; POSIX/Windows variants and indexed ADR links |
| `docs-readme-rewrite` | User reference, development reference, architecture refresh, ADR governance, quality automation, learning | Concise entry point routes only to completed authorities |
| `docs-final-editorial-review` | Every preceding node | Multi-audience findings closed; all gates and primary commands verified |

The repository executes the canonical order even when several nodes become
structurally ready together. Automation waits until the document classes and
authoritative current guides exist, so it validates intended end-state rules
rather than encoding the old layout.

## Audit finding map

| Audit finding | Owner node |
|---|---|
| Windows examples and durability overclaim | `docs-user-reference` |
| Incomplete prerequisites/consumption path | `docs-user-reference` |
| Current/historical profiling and schema conflict | `docs-development-reference` |
| Architecture/DAG drift | `docs-architecture-refresh` |
| Guided-lab paths and POSIX-only lessons | `docs-learning-cross-platform` |
| Superseded ADRs, missing index, mutable evidence, duplicate anchors | `docs-adr-governance` |
| Monolithic implementation-chronology README | `docs-readme-rewrite` |
| Objective structural and command validation | `docs-quality-automation` |
| Cross-corpus consistency and professional polish | `docs-final-editorial-review` |

## Acceptance criteria

This design node is complete when:

- `documentation-manifest.json` classifies every audited Markdown file exactly
  once and names its owner/disposition;
- the full audit findings map to a remediation node;
- each document class has one named source of truth;
- historical ADR preservation and current-reference correction do not
  conflict;
- the migration order has no missing dependency;
- each remediation node has objective exit evidence and complete dependency
  edges;
- class-specific automation and historical exemptions are explicit;
- an independent technical-writing review finds no actionable design gap;
- the PR changes only this ADR, the documentation manifest, and the
  documentation DAG.

## Non-goals

- Rewriting historical ADR bodies for stylistic uniformity.
- Publishing a generated website or introducing a Node/Ruby documentation
  dependency.
- Generating API reference from headers in this remediation.
- Claiming production readiness or changing product support.
- Combining broad documentation migration into one unreviewable PR.

## Design review record

An independent professional information-architecture review found six gaps:
incomplete dependency edges, duplicated authority, no migration manifest,
ambiguous ADR lifecycle ownership, automation that did not distinguish
historical documents, and acceptance criteria without objective evidence.
All were accepted and corrected.

A focused closure review confirmed the authority boundaries, manifest
coverage, lifecycle rules, class-specific gates, finding map, evidence table,
and dependency graph. It found one final vocabulary mismatch: the manifest
used `create` for the future ADR index while the disposition list omitted it.
The list now includes `create`; no actionable design finding remains.

## References

- [Sequential PR workflow](0006-sequential-pull-request-workflow.md)
- [Need-driven simplicity](0010-need-driven-simplicity.md)
- [Documentation-only CI](0048-documentation-only-ci.md)
- [Audit contract repairs](0063-audit-contract-and-validation-repairs.md)
- [Current architecture](../architecture.md)
- [Implementation dependency DAG](../dependency-dag.md)
- [Learning path](../learning/README.md)
