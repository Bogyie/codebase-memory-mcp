# Configuration Reference

This page documents the configuration files that `codebase-memory-mcp` reads or writes today.

## At a Glance

| Purpose | Path | Format | Notes |
|---|---|---|---|
| Global custom extension mapping | `$XDG_CONFIG_HOME/codebase-memory-mcp/config.json` | JSON | Falls back to `~/.config/codebase-memory-mcp/config.json` when `XDG_CONFIG_HOME` is unset. |
| Per-project extension and design discovery | `{repo_root}/.codebase-memory.json` | JSON | Overrides conflicting global `extra_extensions` entries and optionally classifies design sources. |
| CLI-managed runtime settings | `${CBM_CACHE_DIR:-~/.cache/codebase-memory-mcp}/_config.db` | SQLite | Written by `codebase-memory-mcp config set/reset`. |
| UI settings | `${CBM_CACHE_DIR:-~/.cache/codebase-memory-mcp}/config.json` | JSON | Stores `ui_enabled` and `ui_port`. |
| Global Memory | `${CBM_MEMORY_HOME:-<platform user-data directory>}` | SQLite + files | Durable `raw/`, `wiki/`, export, and Git-sync data; intentionally separate from the project-index cache. |

## 1. Custom File Extension Mapping

Two optional JSON files let you map additional file extensions to built-in languages.

### Global config

Default path:

```text
$XDG_CONFIG_HOME/codebase-memory-mcp/config.json
```

Fallback when `XDG_CONFIG_HOME` is unset:

```text
~/.config/codebase-memory-mcp/config.json
```

### Per-project config

Place this file in the repository root:

```text
.codebase-memory.json
```

### Format

```json
{
  "extra_extensions": {
    ".blade.php": "php",
    ".mjs": "javascript",
    ".twig": "html"
  }
}
```

Notes:

- Extension keys must include the leading dot.
- Language names are case-insensitive.
- Unknown language names are skipped.
- Missing files are ignored.
- If the same extension appears in both files, the per-project file wins.

## 2. Design Context Discovery

The repository-root `.codebase-memory.json` may also customize read-only Design Context indexing:

```json
{
  "design": {
    "documents": ["DESIGN.md", "packages/**/DESIGN.md"],
    "token_sources": ["design/**/*.tokens.json"],
    "resolvers": ["design/**/*.resolver.json"],
    "authoritative": ["DESIGN.md", "design/**/*.json"],
    "generated": ["src/styles/generated/**", "dist/**/*.css"]
  }
}
```

All keys are optional arrays of repository-relative `*`, `**`, and `?` patterns. Without this
section, the index discovers `DESIGN.md`, `*.tokens.json`, `*.resolver.json`, CSS, and SCSS by
convention. `authoritative` and `generated` classify provenance only; no design file is written or
generated. See [Design Context and Token Guide](DESIGN_CONTEXT.md) for ownership, layout, supported
formats, security boundaries, and query examples.

## 3. CLI-Managed Runtime Settings

The `config` subcommand stores runtime settings in a small SQLite database:

```text
${CBM_CACHE_DIR:-~/.cache/codebase-memory-mcp}/_config.db
```

Inspect or change values with the CLI:

```bash
codebase-memory-mcp config list
codebase-memory-mcp config get auto_index
codebase-memory-mcp config set auto_index true
codebase-memory-mcp config set auto_index_limit 50000
codebase-memory-mcp config set auto_watch false
codebase-memory-mcp config reset auto_index
```

Current keys:

| Key | Default | Meaning |
|---|---|---|
| `auto_index` | `false` | Automatically index new projects when an MCP session starts. |
| `auto_index_limit` | `50000` | Maximum file count allowed for automatic indexing of a new project. |
| `auto_watch` | `true` | Register the session project with the background Git watcher for ongoing change detection. This is independent of `auto_index`. |
| `ui-lang` | `auto` | Pin the graph UI language to `en` or `zh`, or use automatic selection. |

## 4. UI Settings

The optional built-in graph UI stores its settings in:

```text
${CBM_CACHE_DIR:-~/.cache/codebase-memory-mcp}/config.json
```

Current format:

```json
{
  "ui_enabled": false,
  "ui_port": 9749
}
```

Notes:

- If the UI-enabled binary has embedded assets and no UI config file exists yet, the UI auto-enables on first run.
- `CBM_CACHE_DIR` changes both the UI config location and the runtime settings database location.

## 5. Environment Variables

These environment variables affect runtime behavior:

| Variable | Default | Description |
|---|---|---|
| `CBM_ALLOWED_ROOT` | *(unset)* | Restrict `index_repository` to paths within this directory. When set, a `repo_path` that resolves (after symlink / `..` resolution) outside this root is refused; unset imposes no restriction. Useful when the server may be driven by an untrusted caller (agentic or multi-tenant deployments). |
| `CBM_CACHE_DIR` | `~/.cache/codebase-memory-mcp` | Override the cache directory used for indexes, `_config.db`, and UI `config.json`. |
| `CBM_CURL_BIN` | trusted `curl` from the platform search path | Select an absolute curl executable for release checks and downloads; relative/current-directory executable lookup is rejected. |
| `CBM_DIAGNOSTICS` | `false` | Enable periodic diagnostics output in a private, randomly named system-temp directory. |
| `CBM_DISABLE_UPDATE_CHECK` | `false` | Set to `1` or `true` to disable the bounded background GitHub release-metadata check after MCP initialization. Explicit `update` commands are unaffected. |
| `CBM_DOWNLOAD_URL` | GitHub releases | Override the update download URL. |
| `CBM_DUMP_VERIFY_MIN_RATIO` | `0.5` | Minimum persisted-to-committed node ratio before an index is marked degraded; range `0`–`1`, with `0` disabling the check. |
| `CBM_GIT_BIN` | trusted `git` from the platform search path | Absolute executable used for repository probes and `memory_sync`. Relative and current-directory executable lookup is rejected. |
| `CBM_LOG_LEVEL` | `info` | Set stderr log level to `debug`, `info`, `warn`, `error`, or `none` (or `0`-`4`). |
| `CBM_MAX_FILE_BYTES` | `536870912` | Maximum source-file size read by the indexer, in bytes. Larger files are reported as oversized rather than silently dropped. |
| `CBM_MEMORY_HOME` | platform user-data directory | Override the durable Global Memory root. Defaults to `XDG_DATA_HOME/codebase-memory-mcp/memory` (or `~/.local/share/...`) on Linux, `~/Library/Application Support/.../memory` on macOS, and the local application-data directory on Windows. |
| `CBM_MEMORY_INGEST_ROOTS` | *(unset; path ingest disabled)* | Platform path-list (`:` on POSIX, `;` on Windows) of roots allowed for `memory_ingest`'s `path` input. Inline `content` ingest is unaffected. Only canonical regular, non-symlink files are accepted. |
| `CBM_MEMORY_ALLOW_UNSAFE_PATH_INGEST` | `false` | Set to `1`/`true` only as an explicit operator opt-in to allow path ingest without the roots allowlist. Canonicalization and regular non-symlink checks remain enforced. |
| `CBM_MEM_BUDGET_MB` | auto-detected | Override the indexing memory budget in MiB. Positive values are clamped to detected RAM; invalid values fall back to the RAM-based default. |
| `CBM_SEMANTIC_ENABLED` | `false` | Set to `1` to build `SEMANTICALLY_RELATED` edges during indexing. |
| `CBM_SEMANTIC_THRESHOLD` | built-in threshold | Override the semantic-edge similarity threshold with a value greater than `0` and at most `1`. |
| `CBM_SQLITE_MMAP_SIZE` | `67108864` | SQLite mapping size in bytes for on-disk stores. `0` or a negative value disables mmap; malformed values use the 64 MiB default. |
| `CBM_UI_MAX_RENDER_NODES` | built-in hard limit | Set the maximum node count accepted by an explicit 3D UI render request, up to the built-in hard limit; invalid values use that hard limit. |
| `CBM_WATCHER_PRUNE_GRACE_S` | `600` | Seconds a missing repository root must remain absent before watcher pruning is allowed; `0` removes the time grace but retains the consecutive-poll guard. |
| `CBM_WORKERS` | auto-detected | Override the indexing worker count. |

The opt-in Global Memory scaling probe is a test-runner feature, not a server setting. Run it with
`CBM_RUN_MEMORY_BENCHMARK=1 build/c/test-runner memory`; without that variable the performance probe
is skipped. See [Global Memory Performance Gate](GLOBAL_MEMORY_PERFORMANCE.md).

## 6. Global Memory Write and Sharing Authority

Global Memory keeps durable writes and file sharing explicit:

- `memory_commit` requires `user_approved: true`; omission or `false` is rejected before the proposal is consumed.
- `memory_export` and `memory_import` use the managed bundle below `CBM_MEMORY_HOME` when `path` is omitted.
- A `path` outside the Memory home requires both `allow_external_path: true` and `user_approved: true`.
- Export refuses to replace any existing destination unless both `overwrite: true` and `user_approved: true` are present.
- Export destinations and import sources must be regular, non-symlink files with safe canonical parents.
- Bundles contain retained immutable raw source objects. `memory_sync push` can send that bundle to its configured Git remote, so use export/sync only when the current task explicitly authorizes sharing.

## 7. Agent and Editor Integration Files

The `install` command can write supported MCP entries, instruction blocks, and hooks for 13 detected
agents/editors: Claude Code, Codex CLI, Gemini CLI, Zed, OpenCode, Antigravity, Aider, KiloCode,
VS Code, Cursor, OpenClaw, Kiro, and Junie.

Those target paths vary by tool and platform, so the easiest way to inspect the exact files for your machine is:

```bash
codebase-memory-mcp install --dry-run
```

That prints the specific config files the installer would modify without writing anything.
