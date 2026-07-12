# Global Memory Performance Gate

Global Memory currently rebuilds its graph and FTS projection after a successful semantic write.
This is intentionally simple and resistant to projection drift, but its cost must be measured as
the knowledge base grows.

## Run the scaling probe

Build the C test runner, then opt in to the benchmark:

```bash
make -f Makefile.cbm build/c/test-runner -j4
CBM_RUN_MEMORY_BENCHMARK=1 build/c/test-runner memory
```

Without `CBM_RUN_MEMORY_BENCHMARK=1`, the probe exits immediately so normal unit and CI runs do not
carry a performance workload.

The probe commits claims in bounded batches, explicitly rebuilds at 100 and 1,000 projected
documents, prints both timings, and rejects a clearly superlinear regression using a deliberately
generous relative envelope rather than a machine-specific absolute timeout.

## Current baseline

On 2026-07-12, the sanitizer-enabled local test build produced:

```text
entities=100  rebuild=13 ms
entities=1000 rebuild=108 ms
```

This is approximately linear over the measured range. It does not currently justify replacing the
full rebuild with a more failure-prone incremental projection.

`memory_status` also reports per-process operational evidence:

- `projection.strategy`
- `projection.runs_in_process`
- `projection.last_rebuild_ms`
- `projection.last_rebuild_documents`
- current projection document, node, and edge counts

The built-in UI loads these counters only after the user selects **Inspect Memory**.

## Redesign trigger

Revisit incremental projection when representative workloads show any of the following:

- p95 projection rebuild exceeds 250 ms during ordinary writes;
- 5,000 projected documents no longer remain approximately linear;
- raw source verification dominates writes because total retained bytes, rather than entities,
  becomes the primary cost;
- writer transactions create observable contention or WAL growth.

Before changing the projection algorithm, extend the probe with representative raw source sizes
and concurrent readers. If incremental projection is adopted, compare it against a clean full
rebuild in differential tests so missing documents, stale FTS rows, and orphan graph edges cannot
silently accumulate.
