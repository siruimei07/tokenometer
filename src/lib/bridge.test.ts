import { describe, expect, it, vi } from "vitest";

import {
  HISTORY_REVISION_EVENT,
  LIVE_REVISION_EVENT,
  MockBridge,
  TauriBridge,
  initializeBridge,
  type BridgeTransport,
} from "./bridge";
import type { BootstrapView } from "./contracts";

const bootstrap = {
  historyRevision: 3,
  liveRevision: 1,
  deviceId: "device-1",
  reportingTimeZone: {
    id: "Asia/Shanghai",
    displayName: "Asia/Shanghai",
    source: "windowsSystem",
  },
  implementedCapabilities: [],
  runtimeHealth: [],
} satisfies BootstrapView;

describe("bridge initialization", () => {
  it("registers both listeners before requesting bootstrap", async () => {
    const order: string[] = [];
    const unlistenHistory = vi.fn();
    const unlistenLive = vi.fn();

    const transport: BridgeTransport = {
      async invoke<T>(command: string) {
        order.push(`invoke:${command}`);
        return bootstrap as unknown as T;
      },
      async listen<T>(eventName: string, handler: (payload: T) => void) {
        void handler;
        order.push(`listen:${eventName}`);
        return eventName === HISTORY_REVISION_EVENT
          ? unlistenHistory
          : unlistenLive;
      },
    };

    const connection = await initializeBridge(new TauriBridge(transport), {
      onHistoryRevision: vi.fn(),
      onLiveRevision: vi.fn(),
    });

    expect(order).toEqual([
      `listen:${HISTORY_REVISION_EVENT}`,
      `listen:${LIVE_REVISION_EVENT}`,
      "invoke:get_bootstrap",
    ]);
    expect(connection.bootstrap).toEqual(bootstrap);

    connection.dispose();
    connection.dispose();
    expect(unlistenLive).toHaveBeenCalledOnce();
    expect(unlistenHistory).toHaveBeenCalledOnce();
  });

  it("cleans up an earlier listener when later initialization fails", async () => {
    const unlistenHistory = vi.fn();
    const transport: BridgeTransport = {
      async invoke<T>() {
        return bootstrap as unknown as T;
      },
      async listen<T>(eventName: string, handler: (payload: T) => void) {
        void handler;
        if (eventName === LIVE_REVISION_EVENT) {
          throw new Error("live listener failed");
        }
        return unlistenHistory;
      },
    };

    await expect(
      initializeBridge(new TauriBridge(transport), {
        onHistoryRevision: vi.fn(),
        onLiveRevision: vi.fn(),
      }),
    ).rejects.toThrow("live listener failed");
    expect(unlistenHistory).toHaveBeenCalledOnce();
  });

  it("stops mock event delivery after disposal", async () => {
    const bridge = new MockBridge(bootstrap);
    const onHistoryRevision = vi.fn();
    const onLiveRevision = vi.fn();
    const connection = await initializeBridge(bridge, {
      onHistoryRevision,
      onLiveRevision,
    });

    bridge.emitHistoryRevision({ historyRevision: 4, domains: ["usage"] });
    bridge.emitLiveRevision({ liveRevision: 2, domains: ["host"] });
    connection.dispose();
    bridge.emitHistoryRevision({ historyRevision: 5, domains: ["sessions"] });
    bridge.emitLiveRevision({ liveRevision: 3, domains: ["processes"] });

    expect(onHistoryRevision).toHaveBeenCalledOnce();
    expect(onLiveRevision).toHaveBeenCalledOnce();
    expect(bridge.calls.slice(0, 3)).toEqual([
      `listen:${HISTORY_REVISION_EVENT}`,
      `listen:${LIVE_REVISION_EVENT}`,
      "get_bootstrap",
    ]);
  });
});
