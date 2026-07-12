import { useEffect, useState } from "react";

export type UiLanguage = "en" | "zh";

export const messages = {
  en: {
    tabs: {
      graph: "Graph",
      design: "Design",
      projects: "Projects",
      memory: "Memory",
      control: "Control",
    },
    common: {
      cancel: "Cancel",
      refresh: "Refresh",
      loading: "Loading...",
      save: "Save",
      saving: "Saving...",
      delete: "Delete",
      noMatches: "No matches",
      dismiss: "Dismiss",
    },
    graph: {
      selectedLabel: "Graph",
      search: "Search...",
      clearSelection: "Clear selection",
      folders: "Folders",
    },
    design: {
      eyebrow: "Semantic design context",
      title: "Design systems, tokens, and usage",
      description: "Inspect the repository's portable design definition and trace authoritative tokens into generated CSS and implementation usage.",
      localBoundary: "Repository-local · Global Memory stays separate",
      selectProject: "Select a project to inspect its design context.",
      systems: "Systems",
      tokens: "Tokens",
      components: "Components",
      modes: "Modes",
      relations: "Relations",
      scopes: "Design scopes",
      allScopes: "All scopes",
      searchTokens: "Search token names and paths...",
      emptyTitle: "No Design Context indexed yet",
      tokenDetail: "Token detail",
      value: "Value",
      source: "Source",
      descriptionLabel: "Description",
      connected: "Connected context",
      noRelations: "No connected usages or aliases.",
      noTokens: "No tokens match this filter.",
    },
    memory: {
      title: "Global Memory",
      subtitle: "Search cross-project knowledge with applicability, freshness, evidence, and conflict signals.",
      readOnly: "This screen is intentionally read-only. Durable changes still require an explicit memory_propose → memory_commit workflow.",
      contextLabel: "Context",
      overview: "Overview",
      currentProject: "Current project",
      projectPlaceholder: "Optional project context",
      searchPlaceholder: "What prior fact, decision, or experience is relevant?",
      impact: "Impact",
      freshness: "Freshness",
      preferCurrent: "Prefer current",
      requireCurrent: "Require current",
      search: "Search memory",
      searching: "Searching...",
      route: "Recommended route",
      routeHelp: {
        reuse: "Applicable and sufficiently supported; reuse with its recorded scope.",
        verify: "Potentially useful, but freshness, evidence, or code references need checking.",
        experiment: "Treat this as a hypothesis and validate it with a bounded experiment.",
        deliberate: "Material conflict or high-impact uncertainty requires explicit comparison.",
        abstain: "Do not force a memory match; solve from current evidence instead.",
      },
      snapshot: "Snapshot",
      warnings: "Warnings",
      results: "Results",
      noResults: "No applicable memory found. The safe route is to continue without reuse.",
      audit: "Memory health",
      runAudit: "Run lint audit",
      auditing: "Auditing...",
      healthy: "No lint issues found",
      issues: "issues",
      details: "Raw details",
    },
    projects: {
      indexedProjects: "Indexed Projects",
      noIndexedProjects: "No indexed projects",
      indexFirstRepository: "Index your first repository",
      viewGraph: "View Graph",
      nodes: "nodes",
      edges: "edges",
      deleteTitle: "Delete index",
      deleteConfirm: (name: string) => `Delete index for "${name}"?`,
      healthHealthy: "Database healthy",
      healthMissing: "Database missing",
      healthCorrupt: "Database unhealthy",
      healthChecking: "Checking...",
      indexingInProgress: "Indexing in progress",
      indexingFailed: "Indexing failed",
    },
    index: {
      newIndex: "New Index",
      selectRepositoryFolder: "Select Repository Folder",
      instructions: "Navigate to the project root and click \"Index This Folder\".",
      repositoryPath: "Repository path",
      projectName: "Project ID (optional — permanent, cannot be renamed)",
      projectNamePlaceholder: "Derived from folder name if blank",
      projectNameHelp: "Becomes the database name and query prefix. Leave blank to derive it from the path.",
      filterFolders: "Filter folders",
      noSubdirectories: "No subdirectories",
      indexThisFolder: "Index This Folder",
      starting: "Starting...",
      browseRoot: (path: string) => `Browse ${path}`,
      indexDirectory: (name: string) => `Index ${name}`,
    },
    adr: {
      title: "Architecture Decision Record",
      lastUpdated: "Last updated",
    },
    control: {
      panel: "Control Panel",
      totalCpu: "Total CPU",
      totalRam: "Total RAM",
      processes: "Processes",
      selfRam: "Self RAM",
      activeProcesses: "Active Processes",
      processLogs: "Process Logs",
      noProcesses: "No processes found",
      noLogs: "No logs yet",
      kill: "Kill",
      thisProcess: "THIS",
      uptime: "Uptime",
      killConfirm: (pid: number) => `Kill process ${pid}?`,
    },
  },
  zh: {
    tabs: {
      graph: "图谱",
      design: "设计",
      projects: "项目",
      memory: "记忆",
      control: "控制",
    },
    common: {
      cancel: "取消",
      refresh: "刷新",
      loading: "加载中...",
      save: "保存",
      saving: "保存中...",
      delete: "删除",
      noMatches: "无匹配结果",
      dismiss: "关闭",
    },
    graph: {
      selectedLabel: "图谱",
      search: "搜索...",
      clearSelection: "清除选择",
      folders: "目录",
    },
    design: {
      eyebrow: "语义设计上下文",
      title: "设计系统、令牌与使用关系",
      description: "查看仓库中的可移植设计定义，并追踪权威令牌到生成的 CSS 与实现使用位置。",
      localBoundary: "仅限仓库 · 全局记忆保持独立",
      selectProject: "请选择项目以查看设计上下文。",
      systems: "系统",
      tokens: "令牌",
      components: "组件",
      modes: "模式",
      relations: "关系",
      scopes: "设计范围",
      allScopes: "全部范围",
      searchTokens: "搜索令牌名称与路径...",
      emptyTitle: "尚未索引设计上下文",
      tokenDetail: "令牌详情",
      value: "值",
      source: "来源",
      descriptionLabel: "说明",
      connected: "关联上下文",
      noRelations: "没有关联的使用位置或别名。",
      noTokens: "没有符合筛选条件的令牌。",
    },
    memory: {
      title: "全局记忆",
      subtitle: "结合适用性、时效性、证据和冲突信号搜索跨项目知识。",
      readOnly: "此页面有意设为只读。持久变更仍需显式执行 memory_propose → memory_commit 流程。",
      contextLabel: "上下文",
      overview: "概览",
      currentProject: "当前项目",
      projectPlaceholder: "可选项目上下文",
      searchPlaceholder: "哪些既有事实、决策或经验与当前任务相关？",
      impact: "影响",
      freshness: "时效性",
      preferCurrent: "优先当前信息",
      requireCurrent: "必须为当前信息",
      search: "搜索记忆",
      searching: "搜索中...",
      route: "建议路径",
      routeHelp: {
        reuse: "适用且证据充分；仅在记录的范围内复用。",
        verify: "可能有用，但需先检查时效性、证据或代码引用。",
        experiment: "将其视为假设，并通过有边界的实验验证。",
        deliberate: "存在重大冲突或高影响不确定性，需要显式比较。",
        abstain: "不要强行匹配记忆；改用当前证据解决。",
      },
      snapshot: "快照",
      warnings: "警告",
      results: "结果",
      noResults: "未找到适用记忆。安全路径是不复用并继续当前任务。",
      audit: "记忆健康",
      runAudit: "运行 lint 审计",
      auditing: "审计中...",
      healthy: "未发现 lint 问题",
      issues: "个问题",
      details: "原始详情",
    },
    projects: {
      indexedProjects: "已索引项目",
      noIndexedProjects: "暂无已索引项目",
      indexFirstRepository: "索引第一个仓库",
      viewGraph: "查看图谱",
      nodes: "节点",
      edges: "边",
      deleteTitle: "删除索引",
      deleteConfirm: (name: string) => `删除 "${name}" 的索引？`,
      healthHealthy: "数据库正常",
      healthMissing: "数据库缺失",
      healthCorrupt: "数据库异常",
      healthChecking: "检查中...",
      indexingInProgress: "正在索引",
      indexingFailed: "索引失败",
    },
    index: {
      newIndex: "新建索引",
      selectRepositoryFolder: "选择仓库目录",
      instructions: "导航到项目根目录，然后点击“索引此目录”。",
      repositoryPath: "仓库路径",
      projectName: "项目 ID（可选，永久且不可重命名）",
      projectNamePlaceholder: "留空则从路径派生",
      projectNameHelp: "将作为数据库名称与查询前缀；留空则从路径派生。",
      filterFolders: "筛选目录",
      noSubdirectories: "没有子目录",
      indexThisFolder: "索引此目录",
      starting: "启动中...",
      browseRoot: (path: string) => `浏览 ${path}`,
      indexDirectory: (name: string) => `索引 ${name}`,
    },
    adr: {
      title: "架构决策记录",
      lastUpdated: "最后更新",
    },
    control: {
      panel: "控制面板",
      totalCpu: "总 CPU",
      totalRam: "总内存",
      processes: "进程",
      selfRam: "自身内存",
      activeProcesses: "活动进程",
      processLogs: "进程日志",
      noProcesses: "未找到进程",
      noLogs: "暂无日志",
      kill: "结束",
      thisProcess: "本进程",
      uptime: "运行时间",
      killConfirm: (pid: number) => `结束进程 ${pid}？`,
    },
  },
} as const;

export type UiMessages = (typeof messages)[UiLanguage];

export function detectLanguage(acceptLanguage?: string | null, override?: string | null): UiLanguage {
  if (override === "zh" || override === "en") return override;
  if (!acceptLanguage) return "en";
  const normalized = acceptLanguage.toLowerCase();
  return normalized.includes("zh-cn") || normalized.includes("zh") ? "zh" : "en";
}

let cachedLanguage: UiLanguage = "en";
let languageLoaded = false;
let languageRequest: Promise<UiLanguage> | null = null;
const languageListeners = new Set<(lang: UiLanguage) => void>();

function loadUiLanguage(): Promise<UiLanguage> {
  if (languageLoaded) return Promise.resolve(cachedLanguage);
  if (languageRequest) return languageRequest;

  languageRequest = fetch("/api/ui-config")
    .then((r) => r.json())
    .then((data) => detectLanguage(null, data?.lang))
    .catch(() => detectLanguage(navigator.language))
    .then((lang) => {
      cachedLanguage = lang;
      languageLoaded = true;
      for (const listener of languageListeners) listener(lang);
      return lang;
    })
    .finally(() => {
      languageRequest = null;
    });

  return languageRequest;
}

export function useUiMessages(): UiMessages {
  const [lang, setLang] = useState<UiLanguage>(cachedLanguage);

  useEffect(() => {
    let cancelled = false;
    languageListeners.add(setLang);
    void loadUiLanguage().then((nextLang) => {
      if (!cancelled) setLang(nextLang);
    });
    return () => {
      cancelled = true;
      languageListeners.delete(setLang);
    };
  }, []);

  return messages[lang];
}
