/* JSON-RPC client — speaks the same protocol as MCP clients via POST /rpc */

let _nextId = 1;

export class RpcError extends Error {
  constructor(
    public code: number,
    message: string,
  ) {
    super(message);
    this.name = "RpcError";
  }
}

function mcpErrorMessage(text: unknown): string {
  if (typeof text !== "string" || !text.trim()) return "MCP tool call failed";

  try {
    const value = JSON.parse(text) as Record<string, unknown>;
    if (typeof value.message === "string" && value.message.trim()) return value.message;
    if (typeof value.error === "string" && value.error.trim()) return value.error;
  } catch {
    // MCP errors may be returned as plain text rather than structured JSON.
  }

  return text;
}

export async function callTool<T = unknown>(
  name: string,
  args: Record<string, unknown> = {},
): Promise<T> {
  const res = await fetch("/rpc", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      jsonrpc: "2.0",
      id: _nextId++,
      method: "tools/call",
      params: { name, arguments: args },
    }),
  });

  if (!res.ok) {
    throw new RpcError(-1, `HTTP ${res.status}: ${res.statusText}`);
  }

  const json = await res.json();

  if (json.error) {
    throw new RpcError(json.error.code ?? -1, json.error.message ?? "unknown");
  }

  /* MCP tool results are wrapped: { result: { content: [{ text: "..." }] } } */
  const text = json?.result?.content?.[0]?.text;
  if (json?.result?.isError === true) {
    throw new RpcError(-1, mcpErrorMessage(text));
  }
  if (text === undefined) {
    return json.result as T;
  }

  return JSON.parse(text) as T;
}
