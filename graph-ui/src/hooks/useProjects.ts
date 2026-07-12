import { useCallback, useEffect, useState } from "react";
import { callTool } from "../api/rpc";
import type { Project, SchemaInfo } from "../lib/types";

interface ProjectInfo {
  project: Project;
  schema: SchemaInfo | null;
  status: IndexStatus | null;
}

export interface IndexStatus {
  project: string;
  status: "ready" | "empty" | "no_project";
  nodes: number;
  edges: number;
  root_path?: string;
  indexed_at?: string;
  snapshot_complete: boolean;
  index_generation: string;
}

interface UseProjectsResult {
  projects: ProjectInfo[];
  loading: boolean;
  error: string | null;
  refresh: () => void;
}

export function useProjects(): UseProjectsResult {
  const [projects, setProjects] = useState<ProjectInfo[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  const fetchProjects = useCallback(async () => {
    setLoading(true);
    setError(null);
    try {
      const result = await callTool<{ projects: Project[] }>("list_projects");
      const list = result.projects ?? [];

      /* Fetch schema for each project */
      const infos: ProjectInfo[] = await Promise.all(
        list.map(async (p) => {
          const [schema, status] = await Promise.all([
            callTool<SchemaInfo>("get_graph_schema", { project: p.name }).catch(() => null),
            callTool<IndexStatus>("index_status", { project: p.name }).catch(() => null),
          ]);
          return { project: p, schema, status };
        }),
      );

      setProjects(infos);
    } catch (e) {
      setError(e instanceof Error ? e.message : "Failed to fetch projects");
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    fetchProjects();
  }, [fetchProjects]);

  return { projects, loading, error, refresh: fetchProjects };
}
