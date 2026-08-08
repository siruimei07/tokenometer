import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";

import type {
  AcceptedRevision,
  BootstrapView,
  HistoryRevisionEvent,
  LiveRevisionEvent,
} from "./contracts";

export const HISTORY_REVISION_EVENT = "tokenometer://history-revision";
export const LIVE_REVISION_EVENT = "tokenometer://live-revision";

export type Unlisten = () => void;

export interface AppBridge {
  getBootstrap(): Promise<BootstrapView>;
  refreshNow(scope: "codex"): Promise<AcceptedRevision>;
  listenHistoryRevision(
    handler: (event: HistoryRevisionEvent) => void,
  ): Promise<Unlisten>;
  listenLiveRevision(
    handler: (event: LiveRevisionEvent) => void,
  ): Promise<Unlisten>;
}

export interface BridgeTransport {
  invoke<T>(command: string, args?: Record<string, unknown>): Promise<T>;
  listen<T>(eventName: string, handler: (payload: T) => void): Promise<Unlisten>;
}

const tauriTransport: BridgeTransport = {
  invoke<T>(command: string, args?: Record<string, unknown>) {
    return invoke<T>(command, args);
  },
  listen<T>(eventName: string, handler: (payload: T) => void) {
    return listen<T>(eventName, ({ payload }) => handler(payload));
  },
};

export class TauriBridge implements AppBridge {
  constructor(private readonly transport: BridgeTransport = tauriTransport) {}

  getBootstrap() {
    return this.transport.invoke<BootstrapView>("get_bootstrap");
  }

  refreshNow(scope: "codex") {
    return this.transport.invoke<AcceptedRevision>("refresh_now", { scope });
  }

  listenHistoryRevision(handler: (event: HistoryRevisionEvent) => void) {
    return this.transport.listen(HISTORY_REVISION_EVENT, handler);
  }

  listenLiveRevision(handler: (event: LiveRevisionEvent) => void) {
    return this.transport.listen(LIVE_REVISION_EVENT, handler);
  }
}

type RevisionHandlers = {
  onHistoryRevision: (event: HistoryRevisionEvent) => void;
  onLiveRevision: (event: LiveRevisionEvent) => void;
};

export type BridgeConnection = {
  bootstrap: BootstrapView;
  dispose: () => void;
};

export async function initializeBridge(
  bridge: AppBridge,
  handlers: RevisionHandlers,
): Promise<BridgeConnection> {
  const unlisteners: Unlisten[] = [];
  let disposed = false;

  const dispose = () => {
    if (disposed) return;
    disposed = true;

    for (const unlisten of unlisteners.reverse()) {
      try {
        unlisten();
      } catch {
        // Listener cleanup is best-effort, but one faulty callback must not
        // prevent the remaining local listeners from being removed.
      }
    }
  };

  try {
    unlisteners.push(
      await bridge.listenHistoryRevision(handlers.onHistoryRevision),
    );
    unlisteners.push(await bridge.listenLiveRevision(handlers.onLiveRevision));

    const bootstrap = await bridge.getBootstrap();
    return { bootstrap, dispose };
  } catch (error) {
    dispose();
    throw error;
  }
}

function createMockBootstrap(): BootstrapView {
  const updatedAt = Date.now();
  const timeZone = Intl.DateTimeFormat().resolvedOptions().timeZone;

  return {
    historyRevision: 0,
    liveRevision: 0,
    deviceId: "mock-device",
    reportingTimeZone: {
      id: timeZone,
      displayName: timeZone,
      source: "windowsSystem",
    },
    implementedCapabilities: [],
    runtimeHealth: [
      { runtime: "usage", status: "notStarted", updatedAt },
      { runtime: "live", status: "notStarted", updatedAt },
      { runtime: "limits", status: "notStarted", updatedAt },
    ],
  };
}

export class MockBridge implements AppBridge {
  readonly calls: string[] = [];

  private readonly historyListeners = new Set<
    (event: HistoryRevisionEvent) => void
  >();

  private readonly liveListeners = new Set<
    (event: LiveRevisionEvent) => void
  >();

  constructor(private readonly bootstrap = createMockBootstrap()) {}

  async getBootstrap() {
    this.calls.push("get_bootstrap");
    return this.bootstrap;
  }

  async refreshNow(scope: "codex") {
    this.calls.push(`refresh_now:${scope}`);
    return {
      historyRevision: this.bootstrap.historyRevision,
      accepted: true,
    };
  }

  async listenHistoryRevision(handler: (event: HistoryRevisionEvent) => void) {
    this.calls.push(`listen:${HISTORY_REVISION_EVENT}`);
    this.historyListeners.add(handler);
    let listening = true;

    return () => {
      if (!listening) return;
      listening = false;
      this.historyListeners.delete(handler);
      this.calls.push(`unlisten:${HISTORY_REVISION_EVENT}`);
    };
  }

  async listenLiveRevision(handler: (event: LiveRevisionEvent) => void) {
    this.calls.push(`listen:${LIVE_REVISION_EVENT}`);
    this.liveListeners.add(handler);
    let listening = true;

    return () => {
      if (!listening) return;
      listening = false;
      this.liveListeners.delete(handler);
      this.calls.push(`unlisten:${LIVE_REVISION_EVENT}`);
    };
  }

  emitHistoryRevision(event: HistoryRevisionEvent) {
    for (const listener of this.historyListeners) listener(event);
  }

  emitLiveRevision(event: LiveRevisionEvent) {
    for (const listener of this.liveListeners) listener(event);
  }
}

function hasTauriRuntime() {
  return (
    typeof window !== "undefined" &&
    "__TAURI_INTERNALS__" in (window as Window & { __TAURI_INTERNALS__?: unknown })
  );
}

export function createBridge(): AppBridge {
  if (hasTauriRuntime()) return new TauriBridge();
  if (import.meta.env.DEV) return new MockBridge();

  const unavailable = () =>
    Promise.reject({
      code: "frontend.tauriRuntimeMissing",
      message: "Tokenometer 必须在受信任的桌面运行时中启动。",
      retryable: false,
    });
  return {
    getBootstrap: unavailable,
    refreshNow: unavailable,
    listenHistoryRevision: unavailable,
    listenLiveRevision: unavailable,
  };
}
