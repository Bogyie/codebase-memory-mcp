/* @vitest-environment jsdom */
import "@testing-library/jest-dom/vitest";
import { cleanup, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { DesignTab } from "./DesignTab";

const callToolMock = vi.fn();
vi.mock("../api/rpc", () => ({
  callTool: (...args: unknown[]) => callToolMock(...args),
}));

const RESPONSE = {
  project: "demo",
  status: "ready",
  boundary: "project-local",
  total: { systems: 1, tokens: 2, components: 1, modes: 0 },
  systems: [
    {
      id: 1,
      name: "Aurora",
      qualified_name: "demo.design.system.root",
      file_path: "DESIGN.md",
      line: 1,
      properties: { scope: "root", provenance: "authoritative" },
    },
  ],
  tokens: [
    {
      id: 2,
      name: "action",
      qualified_name: "demo.design.token.root.color.action",
      file_path: "design/tokens/core.tokens.json",
      line: 1,
      properties: {
        scope: "root",
        token_path: "color.action",
        token_type: "color",
        value: "#123456",
        provenance: "authoritative",
      },
    },
    {
      id: 3,
      name: "sm",
      qualified_name: "demo.design.token.root.spacing.sm",
      file_path: "DESIGN.md",
      line: 8,
      properties: {
        scope: "root",
        token_path: "spacing.sm",
        token_type: "dimension",
        value: "8px",
        provenance: "authoritative",
      },
    },
  ],
  components: [
    {
      id: 4,
      name: "button-primary",
      qualified_name: "demo.design.component.root.button-primary",
      file_path: "DESIGN.md",
      line: 12,
      properties: { scope: "root", provenance: "authoritative" },
    },
  ],
  modes: [],
  relations: [
    {
      type: "USES_TOKEN",
      source: "demo.src.styles.app.__file__",
      source_label: "File",
      target: "demo.design.token.root.color.action",
      target_label: "DesignToken",
      properties: { line: 4 },
    },
  ],
  returned_relations: 1,
  has_more: false,
};

describe("DesignTab", () => {
  beforeEach(() => {
    callToolMock.mockResolvedValue(RESPONSE);
    vi.stubGlobal(
      "fetch",
      vi.fn(async () =>
        new Response(JSON.stringify({ lang: "en" }), {
          status: 200,
          headers: { "Content-Type": "application/json" },
        }),
      ),
    );
  });

  afterEach(() => {
    cleanup();
    callToolMock.mockReset();
    vi.unstubAllGlobals();
  });

  it("loads curated context and filters token cards", async () => {
    render(<DesignTab project="demo" />);
    expect((await screen.findAllByText("color.action")).length).toBeGreaterThan(0);
    expect(screen.getByText("button-primary")).toBeInTheDocument();
    expect(screen.getByText(/USES TOKEN/)).toBeInTheDocument();
    expect(callToolMock).toHaveBeenCalledWith("get_design_context", { project: "demo", limit: 1000 });

    fireEvent.change(screen.getByPlaceholderText("Search token names and paths..."), {
      target: { value: "spacing" },
    });
    expect(screen.getAllByText("spacing.sm").length).toBeGreaterThan(0);
    expect(screen.queryAllByText("color.action")).toHaveLength(0);
  });

  it("does not call the backend without a selected project", () => {
    render(<DesignTab project={null} />);
    expect(screen.getByText("Select a project to inspect its design context.")).toBeInTheDocument();
    expect(callToolMock).not.toHaveBeenCalled();
  });
});
