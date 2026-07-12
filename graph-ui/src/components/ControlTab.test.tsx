/* @vitest-environment jsdom */
import "@testing-library/jest-dom/vitest";
import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { ControlTab } from "./ControlTab";

describe("ControlTab process ownership", () => {
  afterEach(() => {
    cleanup();
    vi.unstubAllGlobals();
  });

  it("offers termination only for owned jobs and sends the per-run job id", async () => {
    const fetchMock = vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
      const url = String(input);
      if (url === "/api/ui-config") {
        return new Response(JSON.stringify({ lang: "en" }), { status: 200 });
      }
      if (url === "/api/logs?lines=200") {
        return new Response(JSON.stringify({ lines: [] }), { status: 200 });
      }
      if (url === "/api/processes") {
        return new Response(JSON.stringify({
          self_rss_mb: 12,
          self_user_cpu_s: 1,
          self_sys_cpu_s: 2,
          processes: [
            {
              pid: 111,
              cpu: 0,
              rss_mb: 10,
              elapsed: "00:01",
              command: "codebase-memory-other",
              is_self: false,
              killable: false,
              job_id: "0",
            },
            {
              pid: 222,
              cpu: 1,
              rss_mb: 20,
              elapsed: "00:02",
              command: "codebase-memory-worker",
              is_self: false,
              killable: true,
              job_id: "73",
            },
          ],
        }), { status: 200 });
      }
      if (url === "/api/process-kill") {
        return new Response(JSON.stringify({ kill_requested: 222, job_id: "73" }), {
          status: 202,
        });
      }
      throw new Error(`Unexpected request: ${url} ${String(init?.method)}`);
    });
    vi.stubGlobal("fetch", fetchMock);
    vi.stubGlobal("confirm", vi.fn(() => true));

    render(<ControlTab />);
    expect(await screen.findByText("PID 111")).toBeInTheDocument();
    expect(screen.getByText("PID 222")).toBeInTheDocument();

    const killButtons = screen.getAllByRole("button", { name: "Kill" });
    expect(killButtons).toHaveLength(1);
    fireEvent.click(killButtons[0]);

    await waitFor(() => {
      const call = fetchMock.mock.calls.find(([input]) => String(input) === "/api/process-kill");
      expect(call).toBeDefined();
      expect(JSON.parse(String(call?.[1]?.body))).toEqual({ pid: 222, job_id: "73" });
    });
  });
});
