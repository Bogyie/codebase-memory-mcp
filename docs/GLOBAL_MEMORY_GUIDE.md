# Using Global Memory

Global Memory is user-local knowledge shared across repositories and MCP sessions. It keeps two
durable layers:

- **Raw sources** are immutable, content-addressed objects with provenance and revision lineage.
- **The wiki** is curated, revisioned knowledge: pages, claims, decisions, experiences,
  preferences, relations, and symbolic code references.

The hidden SQLite database is the authority for revisions, graph relationships, full-text search,
proposals, activity, and maintenance state. Files under `wiki/` are materialized views, not a
second write API. For the complete data and concurrency contracts, see
[Global Memory Architecture](GLOBAL_MEMORY.md).

## When to query it

Use `memory_query` when prior cross-project knowledge could materially affect the current task:
an API convention, an operational lesson, a reusable decision, a user preference, or a known
failure mode. Pass the current project and relevant constraints so applicability can be evaluated.

Do not query it automatically for every task. Repository facts are better answered from the current
code graph, and repository-specific architectural decisions should remain in `manage_adr` unless
the user explicitly promotes them. A zero-result or `abstain` response is useful: it means the agent
should proceed from current evidence instead of forcing a memory match.

```bash
codebase-memory-mcp cli memory_query '{
  "query": "SQLite write concurrency",
  "current_context": {
    "project": "billing-service",
    "runtime": "local daemon"
  },
  "impact": "high",
  "freshness": "require_current",
  "limit": 20
}'
```

Every retrieval returns a `snapshot_epoch`, applicability information, warnings, evidence/conflict
context, and a recommended route:

| Route | Meaning | Expected response |
|---|---|---|
| `reuse` | Applicable and sufficiently supported | Reuse within the recorded scope and cite its source lineage. |
| `verify` | Potentially useful but stale, dirty, weakly supported, or uncertain | Check the current source, API, schema, or referenced code before acting. |
| `experiment` | The best candidate is a hypothesis or experience that may not transfer | Run a bounded, reversible test. |
| `deliberate` | Material conflict or high-impact uncertainty exists | Compare the relevant alternatives and evidence explicitly. |
| `abstain` | No safe applicable candidate exists | Continue from current evidence without memory reuse. |

The route is risk-adaptive. Global Memory does **not** require an opposing view for every retrieval.
That would add noise to simple, well-supported cases. Verification effort instead follows current
applicability, evidence lineage, freshness, detected conflict, task impact, and reversibility.

## Inspect the memory graph

The graph can be queried without a repository project:

```bash
codebase-memory-mcp cli get_graph_schema '{"graph":"memory"}'

codebase-memory-mcp cli query_graph '{
  "graph": "memory",
  "query": "MATCH (c:Claim)-[r]->(s:Source) RETURN c.name, type(r), s.name LIMIT 20"
}'
```

Use `memory_query` for normal retrieval because it applies temporal and epistemic routing.
`query_graph(graph="memory")` is intended for structural inspection and advanced diagnostics; a
raw graph match by itself is not a truth judgment.

## Record knowledge safely

Writes require explicit authorization from the current task. The normal flow separates source
capture from curation, and separates agent reasoning from the short commit transaction.

### 1. Ingest an immutable source

Supply either `content` or `path`. Repeated bytes are deduplicated and do not gain truth weight by
being ingested more than once.

```bash
codebase-memory-mcp cli memory_ingest '{
  "path": "/absolute/path/to/api-v2.md",
  "title": "Payments API v2 specification",
  "origin": "https://docs.example.test/payments/v2",
  "publisher": "Payments team",
  "published_at": "2026-07-01T00:00:00Z",
  "retrieved_at": "2026-07-12T10:00:00Z",
  "media_type": "text/markdown"
}'
```

Keep the returned source ID. If this replaces an earlier source, pass its ID as `revision_of` so
dependent claims can be marked for review.

### 2. Read the current snapshot

Before updating existing entities, retrieve them and record their revisions. An overview provides
the current global epoch:

```bash
codebase-memory-mcp cli memory_query '{"mode":"overview"}'
```

### 3. Propose graph and wiki operations

Use stable IDs, explicit scope, review/volatility metadata, and evidence relations. The source ID
below is a placeholder for the value returned by `memory_ingest`.

```bash
codebase-memory-mcp cli memory_propose '{
  "proposal_id": "proposal:payments-api-v2",
  "base_epoch": 42,
  "agent_id": "codex",
  "session_id": "session-2026-07-12",
  "reason": "Promote the newly published API specification",
  "operations": [
    {
      "type": "upsert_page",
      "page_id": "page:payments-api",
      "slug": "payments-api",
      "title": "Payments API",
      "page_kind": "reference",
      "markdown": "# Payments API\n\nThe current supported contract is v2.\n"
    },
    {
      "type": "add_claim",
      "claim_id": "claim:payments-api-current-version",
      "claim_kind": "fact",
      "status": "active",
      "subject": "Payments API",
      "predicate": "current version",
      "object": "v2",
      "page_id": "page:payments-api",
      "scope": {"service": "payments"},
      "review_after": "2026-10-01T00:00:00Z",
      "volatility": "high",
      "source_ids": ["source:<returned-id>"]
    }
  ]
}'
```

For an update, include the entity's `expected_revision` in the operation or
`expected_revisions` in the proposal. Do not silently regenerate a proposal from an old read.

### 4. Commit with an idempotent operation ID

```bash
codebase-memory-mcp cli memory_commit '{
  "proposal_id": "proposal:payments-api-v2",
  "operation_id": "operation:payments-api-v2",
  "agent_id": "codex",
  "session_id": "session-2026-07-12",
  "user_approved": true
}'
```

If another agent changed the same entity, the commit returns a revision conflict. Re-read the
latest entity, decide whether the changes commute or conflict semantically, and create a new
proposal. Last-write-wins is intentionally unavailable. Retrying the same successful operation ID
is idempotent; reusing it for a different proposal is rejected.

## Keep facts current

Facts are not permanent merely because they were once correct. Use these mechanisms together:

- Record `published_at`, `retrieved_at`, validity intervals, volatility, `review_after`, and
  invalidation conditions where applicable.
- Ingest new editions with `revision_of`; claims supported by the older source become review
  candidates.
- Attach symbolic `CodeRef` entities to claims and decisions. Re-indexing and change detection mark
  linked memory dirty when a referenced file or symbol changes or disappears.
- Query with `freshness="require_current"` for high-impact, hard-to-reverse work.
- Use bitemporal `timeline` and `as_of` modes when the question is historical rather than current.
- Run lint and resolve the evidence problem; do not merely suppress the warning.

```bash
codebase-memory-mcp cli memory_query '{
  "mode": "as_of",
  "id": "claim:payments-api-current-version",
  "valid_at": "2025-06-01T00:00:00Z",
  "known_at": "2025-06-15T00:00:00Z"
}'

codebase-memory-mcp cli memory_lint '{"current_project":"payments-service","limit":100}'
```

Lint checks unsupported facts, inference/fact confusion, stale claims, unresolved contradictions,
temporal overlap, weak lineage, retrieval concentration, single-agent dominance, broken graph
links, dirty CodeRefs, conflicting proposals, and pending wiki materialization.

## UI workspace

Install or run the UI build, open `http://localhost:9749`, and select **Memory**. The workspace is
read-only by design:

- overview counts show the current source/wiki/claim/decision inventory;
- search sends project context, impact, and freshness to `memory_query`;
- route, warnings, applicability, and raw result details remain visible;
- lint is an explicit action and can be scoped to the current project.

The UI does not expose direct wiki editing. This preserves explicit authorization and the
revision-aware `memory_propose` → `memory_commit` workflow for durable changes.

## Export, import, and Git synchronization

Bundles contain logical rows **and the raw source bytes**. Treat every export as potentially
sensitive and inspect the destination repository's visibility before configuring a remote.

```bash
codebase-memory-mcp cli memory_export '{"path":"/secure/path/memory-bundle"}'
codebase-memory-mcp cli memory_import '{"path":"/secure/path/memory-bundle","policy":"reject"}'

codebase-memory-mcp cli memory_sync '{"action":"init"}'
codebase-memory-mcp cli memory_sync '{
  "action": "configure_remote",
  "remote": "git@github.com:example/private-memory.git",
  "branch": "cbm-memory"
}'
codebase-memory-mcp cli memory_sync '{"action":"pull","policy":"reject"}'
```

Available import policies are `reject`, `keep_local`, `keep_remote`, and `newest`. `newest` only
resolves disjoint or unchanged-base revisions; semantic conflicts remain proposals. Git is only
transport—the live database is never replaced and Git text merge is not the conflict resolver.

## Storage and privacy

By default, Memory uses the platform user-data directory, separate from disposable per-project
index caches. Set `CBM_MEMORY_HOME` to override it. The directory contains:

```text
raw/objects/       immutable content-addressed source bytes
wiki/              materialized current Markdown revisions
export/            generated portable bundles
sync/              Git transport worktree
_global_memory.db  authoritative graph, revisions, search, and activity
```

Global Memory stays local unless `memory_export`, `memory_import`, or `memory_sync` is explicitly
requested. Credentials are left to Git credential helpers or SSH agents and are not stored in
Memory metadata.

## Tool summary

| Tool | Purpose |
|---|---|
| `memory_ingest` | Deduplicate and retain an immutable raw source with provenance. |
| `memory_query` | Retrieve with applicability, freshness, evidence, conflicts, history, and a safe route. |
| `memory_propose` | Stage revision-aware graph/wiki operations without holding a write transaction. |
| `memory_commit` | Atomically commit with entity revisions and an idempotent operation ID. |
| `memory_lint` | Audit epistemic, temporal, graph, bias, materialization, and CodeRef health. |
| `memory_export` | Write a deterministic logical bundle including raw objects. |
| `memory_import` | Transactionally merge a bundle under an explicit policy. |
| `memory_sync` | Use Git or GitHub as bundle transport without using Git text merge for semantics. |
