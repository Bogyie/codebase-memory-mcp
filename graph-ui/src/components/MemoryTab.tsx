import { useCallback, useEffect, useMemo, useState } from "react";
import type { FormEvent, ReactNode } from "react";
import { ScrollArea } from "@/components/ui/scroll-area";
import { callTool } from "../api/rpc";
import { useUiMessages } from "../lib/i18n";

type MemoryRoute = "reuse" | "verify" | "experiment" | "deliberate" | "abstain";
type MemoryItem = Record<string, unknown>;

interface MemoryQueryResult {
  ok: boolean;
  mode: string;
  snapshot_epoch: number;
  route?: MemoryRoute;
  warnings?: string[];
  results?: MemoryItem[];
  count?: number;
  counts?: Record<string, number>;
  applicability?: {
    matched?: string[];
    mismatched?: string[];
    unknown?: string[];
  };
}

interface MemoryLintIssue {
  code?: string;
  severity?: string;
  kind?: string;
  id?: string;
  detail?: string;
}

interface MemoryLintResult {
  ok: boolean;
  snapshot_epoch: number;
  issue_count: number;
  issues?: MemoryLintIssue[];
}

interface MemoryTabProps {
  projectContext: string | null;
}

const ROUTE_STYLE: Record<MemoryRoute, string> = {
  reuse: "border-emerald-400/25 bg-emerald-400/[0.07] text-emerald-300",
  verify: "border-amber-400/25 bg-amber-400/[0.07] text-amber-300",
  experiment: "border-cyan-400/25 bg-cyan-400/[0.07] text-cyan-300",
  deliberate: "border-violet-400/25 bg-violet-400/[0.07] text-violet-300",
  abstain: "border-slate-400/25 bg-slate-400/[0.07] text-slate-300",
};

function requireSuccessfulResult<T extends { ok: boolean }>(result: T, fallback: string): T {
  if (result.ok) return result;

  const failure = result as T & { error?: unknown; message?: unknown };
  if (typeof failure.message === "string" && failure.message.trim()) {
    throw new Error(failure.message);
  }
  if (typeof failure.error === "string" && failure.error.trim()) {
    throw new Error(failure.error);
  }
  throw new Error(fallback);
}

function textField(item: MemoryItem, key: string): string | null {
  const value = item[key];
  return typeof value === "string" && value.trim() ? value : null;
}

function itemTitle(item: MemoryItem): string {
  return (
    textField(item, "title") ??
    textField(item, "subject") ??
    textField(item, "name") ??
    textField(item, "qualified_name") ??
    textField(item, "id") ??
    "Memory item"
  );
}

function itemSummary(item: MemoryItem): string | null {
  const subject = textField(item, "subject");
  const predicate = textField(item, "predicate");
  const object = textField(item, "object");
  if (subject && (predicate || object)) return [subject, predicate, object].filter(Boolean).join(" ");
  return (
    textField(item, "summary") ??
    textField(item, "content") ??
    textField(item, "markdown") ??
    textField(item, "observation") ??
    textField(item, "outcome") ??
    textField(item, "chosen_option") ??
    textField(item, "value")
  );
}

function Badge({ children }: { children: ReactNode }) {
  return (
    <span className="rounded-md border border-white/[0.06] bg-white/[0.04] px-1.5 py-0.5 text-[10px] text-foreground/45">
      {children}
    </span>
  );
}

export function MemoryTab({ projectContext }: MemoryTabProps) {
  const t = useUiMessages();
  const [overview, setOverview] = useState<MemoryQueryResult | null>(null);
  const [query, setQuery] = useState("");
  const [project, setProject] = useState(projectContext ?? "");
  const [impact, setImpact] = useState("medium");
  const [freshness, setFreshness] = useState("prefer_current");
  const [result, setResult] = useState<MemoryQueryResult | null>(null);
  const [lint, setLint] = useState<MemoryLintResult | null>(null);
  const [loading, setLoading] = useState(false);
  const [lintLoading, setLintLoading] = useState(false);
  const [overviewError, setOverviewError] = useState<string | null>(null);
  const [searchError, setSearchError] = useState<string | null>(null);
  const [lintError, setLintError] = useState<string | null>(null);

  useEffect(() => setProject(projectContext ?? ""), [projectContext]);

  const loadOverview = useCallback(async () => {
    setOverviewError(null);
    try {
      const next = await callTool<MemoryQueryResult>("memory_query", { mode: "overview" });
      setOverview(requireSuccessfulResult(next, "Failed to load Global Memory overview"));
    } catch (e) {
      setOverview(null);
      setOverviewError(e instanceof Error ? e.message : "Failed to load Global Memory overview");
    }
  }, []);

  useEffect(() => {
    void loadOverview();
  }, [loadOverview]);

  const runSearch = async (event: FormEvent) => {
    event.preventDefault();
    if (!query.trim()) return;
    setLoading(true);
    setResult(null);
    setSearchError(null);
    try {
      const args: Record<string, unknown> = {
        query: query.trim(),
        impact,
        freshness,
        limit: 25,
      };
      if (project.trim()) args.current_context = { project: project.trim() };
      const next = await callTool<MemoryQueryResult>("memory_query", args);
      setResult(requireSuccessfulResult(next, "Global Memory query failed"));
    } catch (e) {
      setSearchError(e instanceof Error ? e.message : "Global Memory query failed");
    } finally {
      setLoading(false);
    }
  };

  const runLint = async () => {
    setLintLoading(true);
    setLint(null);
    setLintError(null);
    try {
      const args: Record<string, unknown> = { limit: 100 };
      if (project.trim()) args.current_project = project.trim();
      const next = await callTool<MemoryLintResult>("memory_lint", args);
      setLint(requireSuccessfulResult(next, "Global Memory lint failed"));
    } catch (e) {
      setLintError(e instanceof Error ? e.message : "Global Memory lint failed");
    } finally {
      setLintLoading(false);
    }
  };

  const countEntries = useMemo(() => Object.entries(overview?.counts ?? {}), [overview]);
  const route = result?.route;

  return (
    <ScrollArea className="h-full">
      <div className="mx-auto max-w-5xl p-8">
        <div className="mb-7 flex flex-wrap items-start justify-between gap-4">
          <div>
            <h1 className="text-[20px] font-semibold text-foreground/90">{t.memory.title}</h1>
            <p className="mt-1 max-w-2xl text-[13px] text-foreground/40">{t.memory.subtitle}</p>
          </div>
          <div className="max-w-sm rounded-xl border border-amber-300/15 bg-amber-300/[0.05] px-4 py-3 text-[11px] leading-relaxed text-amber-100/60">
            {t.memory.readOnly}
          </div>
        </div>

        {overviewError && <div role="alert" className="mb-5 rounded-xl border border-destructive/20 bg-destructive/5 px-4 py-3 text-[12px] text-destructive">{overviewError}</div>}

        {countEntries.length > 0 && (
          <section className="mb-7">
            <div className="mb-3 flex items-center justify-between">
              <h2 className="text-[11px] font-medium uppercase tracking-widest text-foreground/35">{t.memory.overview}</h2>
              <span className="font-mono text-[10px] text-foreground/20">
                {t.memory.snapshot} {overview?.snapshot_epoch ?? 0}
              </span>
            </div>
            <div className="grid grid-cols-2 gap-2 sm:grid-cols-3 lg:grid-cols-5">
              {countEntries.map(([name, count]) => (
                <div key={name} className="rounded-xl border border-border/30 bg-white/[0.02] px-3 py-3">
                  <p className="text-[9px] uppercase tracking-wider text-foreground/25">{name.replace(/_/g, " ")}</p>
                  <p className="mt-1 text-[18px] font-semibold tabular-nums text-foreground/75">{count.toLocaleString()}</p>
                </div>
              ))}
            </div>
          </section>
        )}

        <form onSubmit={runSearch} className="mb-6 rounded-2xl border border-border/40 bg-white/[0.02] p-5">
          <label className="block">
            <span className="mb-2 block text-[10px] font-medium uppercase tracking-widest text-foreground/35">{t.memory.search}</span>
            <textarea
              value={query}
              onChange={(event) => setQuery(event.target.value)}
              placeholder={t.memory.searchPlaceholder}
              rows={3}
              className="w-full resize-none rounded-xl border border-white/[0.07] bg-black/20 px-4 py-3 text-[13px] leading-relaxed text-foreground outline-none placeholder:text-foreground/20 focus:border-primary/35"
            />
          </label>
          <div className="mt-3 grid gap-3 sm:grid-cols-[1fr_150px_170px_auto] sm:items-end">
            <label>
              <span className="mb-1 block text-[9px] uppercase tracking-wider text-foreground/25">{t.memory.currentProject}</span>
              <input
                value={project}
                onChange={(event) => setProject(event.target.value)}
                placeholder={t.memory.projectPlaceholder}
                className="w-full rounded-lg border border-white/[0.06] bg-white/[0.03] px-3 py-2 text-[12px] text-foreground outline-none placeholder:text-foreground/20 focus:border-primary/30"
              />
            </label>
            <label>
              <span className="mb-1 block text-[9px] uppercase tracking-wider text-foreground/25">{t.memory.impact}</span>
              <select value={impact} onChange={(event) => setImpact(event.target.value)} className="w-full rounded-lg border border-white/[0.06] bg-[#0e2028] px-3 py-2 text-[12px] text-foreground/70 outline-none">
                <option value="low">Low</option>
                <option value="medium">Medium</option>
                <option value="high">High</option>
              </select>
            </label>
            <label>
              <span className="mb-1 block text-[9px] uppercase tracking-wider text-foreground/25">{t.memory.freshness}</span>
              <select value={freshness} onChange={(event) => setFreshness(event.target.value)} className="w-full rounded-lg border border-white/[0.06] bg-[#0e2028] px-3 py-2 text-[12px] text-foreground/70 outline-none">
                <option value="prefer_current">{t.memory.preferCurrent}</option>
                <option value="require_current">{t.memory.requireCurrent}</option>
              </select>
            </label>
            <button type="submit" disabled={loading || !query.trim()} className="rounded-lg bg-primary/20 px-4 py-2 text-[12px] font-medium text-primary transition-colors hover:bg-primary/30 disabled:opacity-30">
              {loading ? t.memory.searching : t.memory.search}
            </button>
          </div>
        </form>

        {searchError && <div role="alert" className="mb-5 rounded-xl border border-destructive/20 bg-destructive/5 px-4 py-3 text-[12px] text-destructive">{searchError}</div>}

        {route && (
          <div className={`mb-5 rounded-xl border px-4 py-3 ${ROUTE_STYLE[route]}`}>
            <div className="flex flex-wrap items-baseline justify-between gap-2">
              <p className="text-[10px] font-medium uppercase tracking-widest">{t.memory.route}: <strong className="text-[13px]">{route}</strong></p>
              <span className="font-mono text-[10px] opacity-60">{t.memory.snapshot} {result?.snapshot_epoch}</span>
            </div>
            <p className="mt-1 text-[11px] opacity-65">{t.memory.routeHelp[route]}</p>
          </div>
        )}

        {(result?.warnings?.length ?? 0) > 0 && (
          <div className="mb-5 rounded-xl border border-amber-400/15 bg-amber-400/[0.04] px-4 py-3">
            <p className="mb-2 text-[10px] font-medium uppercase tracking-widest text-amber-300/70">{t.memory.warnings}</p>
            <div className="flex flex-wrap gap-1.5">
              {result?.warnings?.map((warning) => <Badge key={warning}>{warning.replace(/_/g, " ")}</Badge>)}
            </div>
          </div>
        )}

        {result && (
          <section className="mb-8">
            <h2 className="mb-3 text-[11px] font-medium uppercase tracking-widest text-foreground/35">
              {t.memory.results} <span className="text-foreground/20">({result.count ?? result.results?.length ?? 0})</span>
            </h2>
            {(result.results?.length ?? 0) === 0 ? (
              <div className="rounded-xl border border-border/30 bg-white/[0.02] px-5 py-8 text-center text-[12px] text-foreground/30">{t.memory.noResults}</div>
            ) : (
              <div className="space-y-3">
                {result.results?.map((item, index) => {
                  const summary = itemSummary(item);
                  return (
                    <article key={`${textField(item, "id") ?? "memory"}-${index}`} className="rounded-xl border border-border/30 bg-white/[0.02] p-4">
                      <div className="flex flex-wrap items-start justify-between gap-3">
                        <div className="min-w-0">
                          <h3 className="text-[13px] font-semibold text-foreground/80">{itemTitle(item)}</h3>
                          {summary && <p className="mt-1 text-[12px] leading-relaxed text-foreground/45">{summary}</p>}
                        </div>
                        <div className="flex flex-wrap gap-1">
                          {[textField(item, "kind"), textField(item, "status"), textField(item, "applicability_state")].filter(Boolean).map((value) => <Badge key={value!}>{value}</Badge>)}
                        </div>
                      </div>
                      <details className="mt-3 text-[10px] text-foreground/25">
                        <summary className="cursor-pointer hover:text-foreground/45">{t.memory.details}</summary>
                        <pre className="mt-2 max-h-80 overflow-auto rounded-lg border border-white/[0.05] bg-black/20 p-3 text-[10px] leading-relaxed text-foreground/50">{JSON.stringify(item, null, 2)}</pre>
                      </details>
                    </article>
                  );
                })}
              </div>
            )}
          </section>
        )}

        <section className="rounded-2xl border border-border/40 bg-white/[0.02] p-5">
          <div className="flex flex-wrap items-center justify-between gap-3">
            <div>
              <h2 className="text-[12px] font-semibold text-foreground/70">{t.memory.audit}</h2>
              {lint && <p className={`mt-1 text-[11px] ${lint.issue_count === 0 ? "text-emerald-300/60" : "text-amber-300/60"}`}>{lint.issue_count === 0 ? t.memory.healthy : `${lint.issue_count} ${t.memory.issues}`}</p>}
            </div>
            <button onClick={runLint} disabled={lintLoading} className="rounded-lg bg-white/[0.05] px-3 py-2 text-[11px] font-medium text-foreground/50 transition-colors hover:bg-white/[0.08] hover:text-foreground/70 disabled:opacity-30">
              {lintLoading ? t.memory.auditing : t.memory.runAudit}
            </button>
          </div>
          {lintError && <div role="alert" className="mt-4 rounded-xl border border-destructive/20 bg-destructive/5 px-4 py-3 text-[12px] text-destructive">{lintError}</div>}
          {(lint?.issues?.length ?? 0) > 0 && (
            <div className="mt-4 space-y-2">
              {lint?.issues?.map((issue, index) => (
                <div key={`${issue.code ?? "issue"}-${issue.id ?? index}`} className="rounded-lg border border-white/[0.05] bg-black/10 px-3 py-2 text-[11px] text-foreground/40">
                  <div className="flex flex-wrap gap-1.5"><Badge>{issue.severity ?? "warning"}</Badge><Badge>{issue.code ?? "unknown"}</Badge>{issue.kind && <Badge>{issue.kind}</Badge>}</div>
                  <p className="mt-1.5">{issue.detail ?? issue.id ?? "Memory maintenance issue"}</p>
                </div>
              ))}
            </div>
          )}
        </section>
      </div>
    </ScrollArea>
  );
}
