import { useEffect, useMemo, useState } from "react";

import {
  createBridge,
  initializeBridge,
  type AppBridge,
  type BridgeConnection,
} from "../lib/bridge";
import type { AppError, BootstrapView } from "../lib/contracts";

type AppShellProps = {
  bridge?: AppBridge;
};

type Revisions = {
  history: number;
  live: number;
};

function toAppError(error: unknown): AppError {
  if (
    typeof error === "object" &&
    error !== null &&
    "code" in error &&
    typeof error.code === "string" &&
    "message" in error &&
    typeof error.message === "string" &&
    "retryable" in error &&
    typeof error.retryable === "boolean"
  ) {
    return {
      code: error.code,
      message: error.message,
      retryable: error.retryable,
      ...("sourceId" in error && typeof error.sourceId === "string"
        ? { sourceId: error.sourceId }
        : {}),
    };
  }

  return {
    code: "frontend.bridgeUnavailable",
    message: "无法连接本地 Tokenometer 后端。",
    retryable: true,
  };
}

export function AppShell({ bridge }: AppShellProps) {
  const appBridge = useMemo(() => bridge ?? createBridge(), [bridge]);
  const [bootstrap, setBootstrap] = useState<BootstrapView>();
  const [error, setError] = useState<AppError>();
  const [refreshing, setRefreshing] = useState(false);
  const [revisions, setRevisions] = useState<Revisions>({
    history: 0,
    live: 0,
  });

  useEffect(() => {
    let active = true;
    let connection: BridgeConnection | undefined;

    void initializeBridge(appBridge, {
      onHistoryRevision: ({ historyRevision }) => {
        if (!active) return;

        setRevisions((current) => ({
          ...current,
          history: Math.max(current.history, historyRevision),
        }));
      },
      onLiveRevision: ({ liveRevision }) => {
        if (!active) return;

        setRevisions((current) => ({
          ...current,
          live: Math.max(current.live, liveRevision),
        }));
      },
    })
      .then((initialized) => {
        connection = initialized;

        if (!active) {
          initialized.dispose();
          return;
        }

        setBootstrap(initialized.bootstrap);
        setRevisions((current) => ({
          history: Math.max(
            current.history,
            initialized.bootstrap.historyRevision,
          ),
          live: Math.max(current.live, initialized.bootstrap.liveRevision),
        }));
      })
      .catch((reason: unknown) => {
        if (active) setError(toAppError(reason));
      });

    return () => {
      active = false;
      connection?.dispose();
    };
  }, [appBridge]);

  const refreshCodex = async () => {
    setRefreshing(true);
    setError(undefined);
    try {
      const accepted = await appBridge.refreshNow("codex");
      setRevisions((current) => ({
        ...current,
        history: Math.max(current.history, accepted.historyRevision),
      }));
    } catch (reason) {
      setError(toAppError(reason));
    } finally {
      setRefreshing(false);
    }
  };

  return (
    <div className="app-shell">
      <header className="app-header">
        <div>
          <p className="eyebrow">本地优先 · Windows</p>
          <h1>Tokenometer</h1>
        </div>
        <span className="phase-badge">Phase 1</span>
      </header>

      <main className="app-content" id="main-content">
        {error ? (
          <section className="status-panel status-panel--error" role="alert">
            <h2>本地服务不可用</h2>
            <p>{error.message}</p>
            <p className="status-code">错误代码：{error.code}</p>
          </section>
        ) : bootstrap ? (
          <section className="status-panel" aria-labelledby="runtime-title">
            <div>
              <p className="status-kicker">Codex 数据核心已接入</p>
              <h2 id="runtime-title">本地采集服务正在运行</h2>
              <p>
                已启用只读增量采集、SQLite 幂等写入和来源健康检查；用量页面将在下一阶段提供。
              </p>
              <button
                className="refresh-button"
                type="button"
                disabled={refreshing}
                onClick={() => void refreshCodex()}
              >
                {refreshing ? "正在刷新…" : "刷新 Codex 数据"}
              </button>
            </div>

            <dl className="runtime-facts">
              <div>
                <dt>历史 revision</dt>
                <dd>{revisions.history}</dd>
              </div>
              <div>
                <dt>实时 revision</dt>
                <dd>{revisions.live}</dd>
              </div>
              <div>
                <dt>报告时区</dt>
                <dd>{bootstrap.reportingTimeZone.displayName}</dd>
              </div>
              <div>
                <dt>采集状态</dt>
                <dd>
                  {bootstrap.runtimeHealth.find(
                    ({ runtime }) => runtime === "usage",
                  )?.status ?? "notStarted"}
                </dd>
              </div>
            </dl>
          </section>
        ) : (
          <section className="status-panel" role="status" aria-live="polite">
            <p className="status-kicker">正在启动</p>
            <h2>连接本地服务…</h2>
            <p>正在先注册 revision listener，再读取启动状态。</p>
          </section>
        )}
      </main>

      <footer className="app-footer">
        <span>默认无遥测、无云同步</span>
        <span>数据保留在本机</span>
      </footer>
    </div>
  );
}
