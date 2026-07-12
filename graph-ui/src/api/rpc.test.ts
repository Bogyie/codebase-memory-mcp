import { afterEach, describe, expect, it, vi } from "vitest";
import { callTool, RpcError } from "./rpc";

describe("callTool", () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it("throws a normalized RpcError for structured MCP tool errors", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => new Response(JSON.stringify({
      result: {
        content: [{ text: JSON.stringify({ ok: false, error: "query_failed", message: "Memory query failed" }) }],
        isError: true,
      },
    }), { status: 200, headers: { "Content-Type": "application/json" } })));

    await expect(callTool("memory_query", {})).rejects.toEqual(
      expect.objectContaining<RpcError>({
        code: -1,
        name: "RpcError",
        message: "Memory query failed",
      }),
    );
  });

  it("preserves plain-text MCP error messages", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => new Response(JSON.stringify({
      result: {
        content: [{ text: "Global Memory is unavailable" }],
        isError: true,
      },
    }), { status: 200, headers: { "Content-Type": "application/json" } })));

    await expect(callTool("memory_query", {})).rejects.toThrow("Global Memory is unavailable");
  });
});
