/* @vitest-environment jsdom */
import "@testing-library/jest-dom/vitest";
import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { MemoryTab } from "./MemoryTab";

function rpcResult(value: unknown, isError = false): Response {
  return new Response(JSON.stringify({
    result: { content: [{ text: JSON.stringify(value) }], isError },
  }), { status: 200, headers: { "Content-Type": "application/json" } });
}

describe("MemoryTab", () => {
  afterEach(() => {
    cleanup();
    vi.unstubAllGlobals();
  });

  it("shows overview, route-aware results, and explicit lint output", async () => {
    const fetchMock = vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
      if (String(input) === "/api/ui-config") {
        return new Response(JSON.stringify({ lang: "en" }), { status: 200 });
      }
      const request = JSON.parse(String(init?.body));
      const tool = request.params.name;
      const args = request.params.arguments;
      if (tool === "memory_query" && args.mode === "overview") {
        return rpcResult({ ok: true, mode: "overview", snapshot_epoch: 7, counts: { sources: 2, claims: 1 } });
      }
      if (tool === "memory_query") {
        return rpcResult({
          ok: true,
          mode: "search",
          snapshot_epoch: 7,
          route: "verify",
          warnings: ["stale_or_review_due_memory"],
          count: 1,
          results: [{
            kind: "claim",
            id: "claim:api-version",
            subject: "Service API",
            predicate: "uses",
            object: "v2",
            status: "stale",
            applicability_state: "matched",
          }],
        });
      }
      if (tool === "memory_lint") {
        return rpcResult({
          ok: true,
          snapshot_epoch: 7,
          issue_count: 1,
          issues: [{ code: "stale_claim", severity: "warning", kind: "claim", id: "claim:api-version", detail: "Review due" }],
        });
      }
      throw new Error(`Unexpected request: ${tool}`);
    });
    vi.stubGlobal("fetch", fetchMock);

    render(<MemoryTab projectContext="sample-project" />);

    expect(await screen.findByText("sources")).toBeInTheDocument();
    expect(screen.getByDisplayValue("sample-project")).toBeInTheDocument();

    fireEvent.change(screen.getByPlaceholderText("What prior fact, decision, or experience is relevant?"), {
      target: { value: "Which API version should I use?" },
    });
    fireEvent.click(screen.getByRole("button", { name: "Search memory" }));

    expect(await screen.findByText(/Recommended route:/)).toHaveTextContent("verify");
    expect(screen.getByText("stale or review due memory")).toBeInTheDocument();
    expect(screen.getByText("Service API uses v2")).toBeInTheDocument();

    fireEvent.click(screen.getByRole("button", { name: "Run lint audit" }));
    await waitFor(() => expect(screen.getByText("stale_claim")).toBeInTheDocument());

    const searchCall = fetchMock.mock.calls.find(([, init]) => {
      if (!init?.body) return false;
      const body = JSON.parse(String(init.body));
      return body.params?.name === "memory_query" && body.params.arguments?.query;
    });
    const searchBody = JSON.parse(String(searchCall?.[1]?.body));
    expect(searchBody.params.arguments.current_context).toEqual({ project: "sample-project" });
    expect(searchBody.params.arguments.freshness).toBe("prefer_current");
  });

  it("rejects domain failures and removes stale search and lint output", async () => {
    let searchCount = 0;
    let lintCount = 0;
    const fetchMock = vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
      if (String(input) === "/api/ui-config") {
        return new Response(JSON.stringify({ lang: "en" }), { status: 200 });
      }
      const request = JSON.parse(String(init?.body));
      const tool = request.params.name;
      const args = request.params.arguments;
      if (tool === "memory_query" && args.mode === "overview") {
        return rpcResult({ ok: true, mode: "overview", snapshot_epoch: 7, counts: {} });
      }
      if (tool === "memory_query") {
        searchCount += 1;
        if (searchCount === 1) {
          return rpcResult({
            ok: true,
            mode: "search",
            snapshot_epoch: 7,
            route: "reuse",
            count: 1,
            results: [{ id: "claim:old", subject: "Old result" }],
          });
        }
        return rpcResult({ ok: false, error: "query_failed", message: "Current query failed" });
      }
      if (tool === "memory_lint") {
        lintCount += 1;
        if (lintCount === 1) {
          return rpcResult({
            ok: true,
            snapshot_epoch: 7,
            issue_count: 1,
            issues: [{ code: "stale_claim", detail: "Old lint result" }],
          });
        }
        return rpcResult({ ok: false, error: "lint_failed", message: "Current lint failed" });
      }
      throw new Error(`Unexpected request: ${tool}`);
    });
    vi.stubGlobal("fetch", fetchMock);

    render(<MemoryTab projectContext={null} />);
    await waitFor(() => expect(fetchMock).toHaveBeenCalled());

    const queryInput = screen.getByPlaceholderText("What prior fact, decision, or experience is relevant?");
    fireEvent.change(queryInput, { target: { value: "first query" } });
    fireEvent.click(screen.getByRole("button", { name: "Search memory" }));
    expect(await screen.findByText("Old result")).toBeInTheDocument();

    fireEvent.change(queryInput, { target: { value: "second query" } });
    fireEvent.click(screen.getByRole("button", { name: "Search memory" }));
    expect(await screen.findByText("Current query failed")).toBeInTheDocument();
    expect(screen.queryByText("Old result")).not.toBeInTheDocument();
    expect(screen.queryByText(/Recommended route:/)).not.toBeInTheDocument();

    fireEvent.click(screen.getByRole("button", { name: "Run lint audit" }));
    expect(await screen.findByText("Old lint result")).toBeInTheDocument();
    fireEvent.click(screen.getByRole("button", { name: "Run lint audit" }));
    expect(await screen.findByText("Current lint failed")).toBeInTheDocument();
    expect(screen.queryByText("Old lint result")).not.toBeInTheDocument();
    expect(screen.queryByText("undefined issues")).not.toBeInTheDocument();
  });
});
