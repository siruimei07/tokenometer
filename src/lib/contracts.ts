export type AgentProvider = "codex" | "claudeCode" | "openCode";

export type AgentCapability =
  | "usage"
  | "sessions"
  | "liveProcess"
  | "context"
  | "quota"
  | "git"
  | "ports"
  | "mcp"
  | "subagents"
  | "providerMemory"
  | "networkQuota";

export type MeasurementKind =
  | "exact"
  | "estimated"
  | "heuristic"
  | "unavailable";

export type Availability<T> =
  | { state: "available"; value: T; measuredAt: number }
  | {
      state: "stale";
      lastGood: T;
      measuredAt: number;
      staleSince: number;
      reason?: string;
    }
  | {
      state:
        | "unsupported"
        | "notConfigured"
        | "notObserved"
        | "permissionDenied";
      reason?: string;
    };

export type HistoryDomain =
  | "dashboard"
  | "usage"
  | "sessions"
  | "trends"
  | "sources"
  | "quotas"
  | "accounts"
  | "cost"
  | "devices"
  | "settings"
  | "exports"
  | "sync"
  | "alerts";

export type LiveDomain =
  | "host"
  | "liveSessions"
  | "processes"
  | "ports"
  | "mcp"
  | "alerts"
  | "compact";

export type HistoryRevisionEvent = {
  historyRevision: number;
  domains: HistoryDomain[];
};

export type LiveRevisionEvent = {
  liveRevision: number;
  domains: LiveDomain[];
};

export type RuntimeKind = "usage" | "live" | "limits";

export type RuntimeStatus =
  | "notStarted"
  | "starting"
  | "healthy"
  | "degraded"
  | "stopped";

export type RuntimeHealthView = {
  runtime: RuntimeKind;
  status: RuntimeStatus;
  updatedAt: number;
  message?: string;
};

export type ProviderCapabilityView = {
  provider: AgentProvider;
  capabilities: AgentCapability[];
};

export type ReportingTimeZoneView = {
  id: string;
  displayName: string;
  source: "windowsSystem";
};

export type BootstrapView = {
  historyRevision: number;
  liveRevision: number;
  deviceId: string;
  reportingTimeZone: ReportingTimeZoneView;
  implementedCapabilities: ProviderCapabilityView[];
  runtimeHealth: RuntimeHealthView[];
};

export type RefreshScope = "codex";

export type AcceptedRevision = {
  historyRevision: number;
  accepted: boolean;
};

export type Revisioned<T> = {
  asOfHistoryRevision: number;
  data: T;
};

export type LiveRevisioned<T> = {
  asOfLiveRevision: number;
  observedAt: number;
  expiresAt: number;
  data: T;
};

export type DualRevisioned<T> = {
  asOfHistoryRevision: number;
  asOfLiveRevision: number;
  data: T;
};

export type AppError = {
  code: string;
  message: string;
  retryable: boolean;
  sourceId?: string;
};
