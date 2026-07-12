import type { GraphData } from "./types";

export const GRAPH_CANVAS_DPR: [number, number] = [1, 1.5];
export const GRAPH_COMPOSER_MULTISAMPLING = 0;

export function formatGraphLimitNotice(data: GraphData | null): string | null {
  if (!data || data.total_nodes <= data.nodes.length) return null;
  return `Showing ${data.nodes.length.toLocaleString("en-US")} of ${data.total_nodes.toLocaleString("en-US")} nodes (${data.edges.length.toLocaleString("en-US")} edges). Raise the node budget or use filters.`;
}
